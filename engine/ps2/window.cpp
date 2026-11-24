/* the PS2 in place of the SDL3 window (engine/pc/window.h): its screen is
 * the GS's, which gs.cpp opens with the raster, and its pads the game's, by
 * the BIOS's pad driver through libpad. a script of keys, where the settings
 * give one (engine/native/pads.c), plays over the first. SPU2 will take the
 * sound's place here */
#include <stdio.h>

extern "C" {
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>
}

#include "draw.h"
#include "window.h"

namespace pc {

namespace {

/* the two ports a pad plugs into, the game's first and second */
constexpr int PORTS = 2;

/* Shinobi's PDD_DGT bits, as the game reads a pad */
enum {
    DC_B = 1 << 1,
    DC_A = 1 << 2,
    DC_START = 1 << 3,
    DC_UP = 1 << 4,
    DC_DOWN = 1 << 5,
    DC_LEFT = 1 << 6,
    DC_RIGHT = 1 << 7,
    DC_Y = 1 << 9,
    DC_X = 1 << 10,
    DC_TR = 1 << 16,
    DC_TL = 1 << 17
};
/* the PS2's buttons by the place the Dreamcast's pad has each: cross is A
 * and circle B, as each pad's lower and right buttons are, and either
 * shoulder of a side is that side's trigger */
const struct {
    uint16_t button;
    uint32_t bit;
} BUTTONS[] = {
    {PAD_CROSS, DC_A},     {PAD_CIRCLE, DC_B}, {PAD_SQUARE, DC_X},   {PAD_TRIANGLE, DC_Y}, {PAD_START, DC_START},
    {PAD_UP, DC_UP},       {PAD_DOWN, DC_DOWN}, {PAD_LEFT, DC_LEFT}, {PAD_RIGHT, DC_RIGHT}, {PAD_L1, DC_TL},
    {PAD_L2, DC_TL},       {PAD_R1, DC_TR},     {PAD_R2, DC_TR},
};
/* a stick this near its middle rests: a worn one comes back near it, not
 * to it */
constexpr int DEAD = 24;
/* the analog pads' kinds, by the high nibble of a read's mode */
constexpr int KIND_ANALOG = 5, KIND_DUALSHOCK = 7;

/* each port's room for the driver, which the IOP writes by DMA */
char pad_areas[PORTS][256] __attribute__((aligned(64)));

} // namespace

struct Window {
    /* whether the driver opened, and each port's: whether it asked the
     * pad for its sticks since the pad came, and its buttons held */
    bool pads = false;
    bool opened[PORTS] = {};
    bool asked[PORTS] = {};
    uint32_t held[PORTS] = {};
};

/* the pad driver, from the BIOS's own: the serial ports' manager, then the
 * pads' */
Window *window_open(const char *, uint32_t, uint32_t, uint32_t, const char *,
                    const Image *, std::string &)
{
    Window *window = new Window;
    int sio = 0, pad = 0;

    SifInitRpc(0);
    if ((sio = SifLoadModule("rom0:SIO2MAN", 0, nullptr)) < 0 || (pad = SifLoadModule("rom0:PADMAN", 0, nullptr)) < 0 ||
        padInit(0) != 1) {
        fprintf(stderr, "sa1: the pads' driver didn't load: SIO2MAN %d, PADMAN %d\n", sio, pad);
        return window;
    }
    window->pads = true;
    for (int port = 0; port < PORTS; port++) {
        window->opened[port] = padPortOpen(port, 0, pad_areas[port]) != 0;
    }
    fprintf(stderr, "sa1: the pads' driver is open, port 1 %s, port 2 %s\n", window->opened[0] ? "open" : "not",
            window->opened[1] ? "open" : "not");
    return window;
}

void window_close(Window *window)
{
    delete window;
}

void window_alert(const char *title, const char *words)
{
    fprintf(stderr, "sa1: %s: %s\n", title, words);
}

const char *const *window_extensions(uint32_t &count)
{
    count = 0;
    return nullptr;
}

bool window_surface(Window *, void *, uint64_t &)
{
    return false;
}

void window_size(Window *, uint32_t &width, uint32_t &height)
{
    width = 640;
    height = 480;
}

bool window_pump(Window *)
{
    return true;
}

/* a stick's axis from the pad's 0 to 255, its middle 128, as the game's
 * -128 to 127, 0 within the dead zone */
int16_t axis(unsigned char at)
{
    int v = static_cast<int>(at) - 128;

    return static_cast<int16_t>(v > -DEAD && v < DEAD ? 0 : v);
}

sa_result window_pad(Window *window, uint32_t port, sa_pad &pad)
{
    struct padButtonStatus now;

    if (window == nullptr || !window->pads || port >= PORTS || !window->opened[port]) {
        return SA_EMPTY;
    }
    int state = padGetState(static_cast<int>(port), 0);

    /* a pad plugged in settles over some reads first: till then, and with
     * none there, the port holds nothing */
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        window->asked[port] = false;
        window->held[port] = 0;
        return SA_EMPTY;
    }
    /* a DualShock starts with its sticks off: they're turned on, and kept
     * on against its analog button */
    if (!window->asked[port] && state == PAD_STATE_STABLE) {
        window->asked[port] = true;
        if (padInfoMode(static_cast<int>(port), 0, PAD_MODETABLE, -1) > 0) {
            padSetMainMode(static_cast<int>(port), 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        }
    }
    if (padRead(static_cast<int>(port), 0, &now) == 0) {
        return SA_EMPTY;
    }
    /* the pad's buttons read 0 where held */
    uint16_t buttons = static_cast<uint16_t>(0xffff ^ now.btns);
    uint32_t held = 0;

    for (const auto &b : BUTTONS) {
        held |= (buttons & b.button) != 0 ? b.bit : 0;
    }
    pad = sa_pad{};
    pad.head.kind = SA_REC_PAD;
    pad.head.size = sizeof pad;
    pad.present = 1;
    pad.on = held;
    pad.press = held & ~window->held[port];
    pad.release = window->held[port] & ~held;
    window->held[port] = held;
    int kind = now.mode >> 4;

    if (kind == KIND_ANALOG || kind == KIND_DUALSHOCK) {
        pad.x1 = axis(now.ljoy_h);
        pad.y1 = axis(now.ljoy_v);
        pad.x2 = axis(now.rjoy_h);
        pad.y2 = axis(now.rjoy_v);
    }
    /* the directions push the stick whole where it rests, as a pad's do */
    if (pad.x1 == 0 && (held & (DC_LEFT | DC_RIGHT)) != 0) {
        pad.x1 = (held & DC_LEFT) != 0 ? -128 : 127;
    }
    if (pad.y1 == 0 && (held & (DC_UP | DC_DOWN)) != 0) {
        pad.y1 = (held & DC_UP) != 0 ? -128 : 127;
    }
    pad.l = (held & DC_TL) != 0 ? 255 : 0;
    pad.r = (held & DC_TR) != 0 ? 255 : 0;
    return SA_OK;
}

sa_result window_rumble(Window *, uint32_t, uint16_t, uint16_t, uint32_t,
                        uint32_t)
{
    return SA_ERR_UNSUPPORTED;
}

bool window_sound_open(Window *, SoundRender, void *, std::string &why)
{
    why = "the PS2 has no sound yet";
    return false;
}

void window_sound_close(Window *)
{
}

} // namespace pc

extern "C" void psp_sound_report(void)
{
}
