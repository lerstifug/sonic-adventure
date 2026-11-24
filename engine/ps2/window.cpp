/* the PS2 in place of the SDL3 window (engine/pc/window.h): its screen is
 * the GS's, which gs.cpp opens with the raster, so the window holds nothing
 * of its own yet. the pads come from a script of keys where the settings
 * give one (engine/native/pads.c). the PS2's own pads and SPU2 will take
 * their places here */
#include <stdio.h>

#include "draw.h"
#include "window.h"

namespace pc {

struct Window {
    int open = 1;
};

Window *window_open(const char *, uint32_t, uint32_t, uint32_t, const char *,
                    const Image *, std::string &)
{
    return new Window;
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

sa_result window_pad(Window *, uint32_t, sa_pad &)
{
    return SA_ERR_UNSUPPORTED;
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
