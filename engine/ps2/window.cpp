/* the PS2 in place of the SDL3 window (engine/pc/window.h): its screen is
 * the GS's, which gs.cpp opens with the raster, and its pads the game's, by
 * the BIOS's pad driver through libpad. a script of keys, where the settings
 * give one (engine/native/pads.c), plays over the first. SPU2 will take the
 * sound's place here */
#include <stdio.h>

extern "C" {
#include <audsrv.h>
#include <delaythread.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
}

#include "draw.h"
#include "pspkernel.h"
#include "window.h"
#include "spu.h"

/* the run's vsyncs, as the log tells them (engine/native/native.h) */
extern "C" unsigned long native_vsyncs;
/* audsrv's IOP module, which the build makes the program's own bytes
 * (engine/mk/ps2.mk), and the gp the thread takes */
extern "C" unsigned char audsrv_irx[];
extern "C" unsigned int size_audsrv_irx;
extern "C" void *_gp;
/* the lock ps2sdk's file calls take (its fio), which the sound's calls to
 * the IOP take too */
extern "C" int _fio_io_sema;
/* the notes the SPU2's voices took (engine/ps2/sound_spu.h), and whether
 * the settings ask for the trace */
extern "C" unsigned long ps2_spu_notes;
extern "C" int native_trace;

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
    /* the sound: the engine's render, its thread, and what it took */
    volatile bool playing = false;
    SoundRender render = nullptr;
    void *user = nullptr;
    int thread = -1;
    uint64_t render_us = 0;
    uint32_t pieces = 0;
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

    /* the log's word of each change of a port's state, the first few */
    {
        static int last[PORTS] = {-1, -1}, said;

        if (state != last[port] && said < 40) {
            fprintf(stderr, "sa1: pad %u is in state %d, at vsync %lu\n", static_cast<unsigned>(port + 1), state,
                    native_vsyncs);
            last[port] = state;
            said++;
        }
    }
    /* with no pad there, the port holds nothing, and the next pad plugged
     * in is asked for its sticks again. a pad busy with a command, as the
     * sticks' own turns it for a few reads, still holds its buttons: those
     * reads went empty before, and the record kept the last press, so each
     * press counted twice */
    if (state == PAD_STATE_DISCONN) {
        window->asked[port] = false;
        window->held[port] = 0;
        return SA_EMPTY;
    }
    /* a DualShock starts with its sticks off: they're turned on once, and
     * kept on against its analog button */
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

namespace {

/* the sound (engine/ps2/SOUND.md's stage 1): the engine's own render, the
 * PSP's mixer of the AICA's voices and the ADX streams, on a thread of the
 * EE's above the game's, a piece at a time to the IOP's audsrv, which takes
 * 44.1 kHz and gives the SPU2 its 48. the thread waits for audsrv's room,
 * so it takes the EE only while it renders */
constexpr int PIECE = 1024;
constexpr int SOUND_STACK = 32 * 1024;
uint8_t sound_stack[SOUND_STACK] __attribute__((aligned(16)));
Window *sounding = nullptr;

void play(void *arg)
{
    static int16_t piece[PIECE * 2] __attribute__((aligned(64)));
    Window *w = static_cast<Window *>(arg);
    /* the FPU as the engine's C++ has it elsewhere: a result too small for
     * a normal number flushed to zero, no exception enabled. the word is
     * engine/native/native.h's NATIVE_FCR31_HOST */
    uint32_t host = 1U << 24;

    __asm__ volatile("ctc1 %0, $31" : : "r"(host));
    while (w->playing) {
        uint32_t began = sceKernelGetSystemTimeLow();

        w->render(w->user, piece, PIECE);
        w->render_us += sceKernelGetSystemTimeLow() - began;
        w->pieces++;
        /* the log's word, every 5 seconds, of the SPU2's voices: the notes
         * they took, and those of each core with an envelope above 0 */
        if (native_trace && w->pieces % 215 == 0 && ps2::spu_ready()) {
            struct sa1snd_status st {};
            uint32_t live[2] = {0, 0};

            ps2::spu_status(st);
            for (uint32_t i = 0; i < SA1SND_VOICES; i++) {
                live[i / 24] += st.envelope[i] != 0;
            }
            fprintf(stderr, "sa1: the SPU2's voices took %lu notes; %u of core 0's and %u of core 1's sound\n",
                    ps2_spu_notes, static_cast<unsigned>(live[0]), static_cast<unsigned>(live[1]));
        }
        /* the piece goes once audsrv's ring has room for it. each call to
         * the IOP is a short one, under the lock of ps2sdk's file calls,
         * so none crosses the game's: audsrv's own wait, a call held at the
         * IOP till the room comes, froze the game's first file read where
         * the two crossed. between, the thread sleeps on the EE */
        for (;;) {
            bool sent = false;

            WaitSema(_fio_io_sema);
            if (audsrv_available() >= static_cast<int>(sizeof piece)) {
                audsrv_play_audio(reinterpret_cast<const char *>(piece), static_cast<int>(sizeof piece));
                sent = true;
            }
            SignalSema(_fio_io_sema);
            if (sent) {
                break;
            }
            DelayThread(4000);
        }
    }
    ExitThread();
}

} // namespace

bool window_sound_open(Window *window, SoundRender render, void *user, std::string &why)
{
    int ret = 0, sd = 0, srv = 0;
    ee_thread_status_t self{};

    if (window == nullptr || window->playing) {
        why = "the PS2's sound is open already, or it has no window";
        return false;
    }
    /* the SPU2's driver from the BIOS, then audsrv from the program's own
     * bytes, which the BIOS's loader takes only once patched to */
    sbv_patch_enable_lmb();
    if ((sd = SifLoadModule("rom0:LIBSD", 0, nullptr)) < 0 ||
        (srv = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, nullptr, &ret)) < 0 || ret < 0 ||
        audsrv_init() != 0) {
        why = "audsrv didn't load: LIBSD " + std::to_string(sd) + ", audsrv " + std::to_string(srv) + " " +
              std::to_string(ret);
        return false;
    }
    audsrv_fmt_t format{};

    format.bits = 16;
    format.freq = 44100;
    format.channels = 2;
    if (audsrv_set_format(&format) != 0) {
        why = "audsrv took no 44.1 kHz stereo";
        return false;
    }
    audsrv_set_volume(MAX_VOLUME);
    {
        const char *spu_why = "";

        if (ps2::spu_open(spu_why)) {
            fprintf(stderr, "sa1: sa1snd answers: the SPU2's voices are the sound effects'\n");
        } else {
            fprintf(stderr, "sa1: no sa1snd: %s\n", spu_why);
        }
    }
    window->render = render;
    window->user = user;
    window->playing = true;
    /* a step above the thread that opens it, the game's: its render takes
     * a part of the time it fills, and waits out the rest in audsrv's room.
     * the game's starts at 1, the highest a thread takes, and the kernel
     * runs another of the same priority only when the first waits, which
     * the game's spins don't: the game's goes a step down */
    ReferThreadStatus(GetThreadId(), &self);
    int game = self.current_priority;

    if (game <= 1) {
        game = 2;
        ChangeThreadPriority(GetThreadId(), game);
    }
    ee_thread_t thread{};

    thread.func = reinterpret_cast<void *>(play);
    thread.stack = sound_stack;
    thread.stack_size = SOUND_STACK;
    thread.gp_reg = &_gp;
    thread.initial_priority = game - 1;
    if ((window->thread = CreateThread(&thread)) < 0 || StartThread(window->thread, window) < 0) {
        window->playing = false;
        why = "the PS2 started no thread for the sound";
        return false;
    }
    sounding = window;
    fprintf(stderr, "sa1: the sound plays by audsrv, its thread at priority %d over the game's %d\n",
            thread.initial_priority, game);
    return true;
}

void window_sound_close(Window *window)
{
    if (window == nullptr || !window->playing) {
        return;
    }
    window->playing = false;
    audsrv_stop_audio();
}

} // namespace pc

extern "C" void psp_sound_report(void)
{
    const pc::Window *w = pc::sounding;

    if (w != nullptr && w->pieces != 0) {
        fprintf(stderr, "sa1: the sound's render took %u ms of the EE for %u ms of sound\n",
                static_cast<unsigned>(w->render_us / 1000),
                static_cast<unsigned>(static_cast<uint64_t>(w->pieces) * pc::PIECE * 1000 / 44100));
    }
}
