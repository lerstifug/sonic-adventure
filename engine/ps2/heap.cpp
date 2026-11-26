/* the PS2's heap: its room, and a reserve against a request it can't meet.
 *
 * the console's heap holds every texture a stage draws, where the
 * Dreamcast kept them in the PVR's own 8 MB, and the port's caches take the
 * room the game leaves: the model cache (sa1/models.h), the blends' sort and
 * the mirrored copies (gs.cpp), and the TIM2's mip levels (textures.cpp)
 * each give way by the heap's room. a burst of the game's own requests
 * between two looks can still meet a heap the caches filled: Perfect
 * Chaos's opening scene ran it out. so the program holds a reserve, which
 * the link's wraps of malloc and its kin (engine/mk/ps2.mk) give back where
 * a request fails, and try again. the model cache then goes whole at the
 * picture's look (ps2_heap_look), and the reserve comes back once the heap
 * has room for it */
#include <kernel.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern "C" {
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);
}

namespace {

constexpr size_t RESERVE = 1024U << 10;
/* the room past the reserve the heap holds before it's taken back */
constexpr uint32_t RESERVE_AGAIN = 2048U << 10;

void *reserve;
volatile int spent;

/* a request that failed, again with the reserve given back */
bool give_back(size_t asked)
{
    void *r = reserve;

    if (r == nullptr) {
        return false;
    }
    reserve = nullptr;
    free(r);
    spent = 1;
    fprintf(stderr, "sa1: the heap ran short: its reserve of %u KB went to a request of %u bytes\n",
            static_cast<unsigned>(RESERVE / 1024), static_cast<unsigned>(asked));
    return true;
}

} // namespace

extern "C" {

/* the heap's room: its free blocks, and what sbrk can take yet, up to the
 * kernel's end of the heap. a look walks the heap's free blocks */
uint32_t ps2_heap_room(void)
{
    struct mallinfo m = mallinfo();
    uintptr_t top = reinterpret_cast<uintptr_t>(sbrk(0)), end = reinterpret_cast<uintptr_t>(EndOfHeap());

    return static_cast<uint32_t>(m.fordblks) + (end > top ? static_cast<uint32_t>(end - top) : 0U);
}

/* the reserve, taken at the program's start (main.c) */
void ps2_heap_reserve(void)
{
    if (reserve == nullptr) {
        reserve = __real_malloc(RESERVE);
    }
}

/* once a picture (sa1/models.h's look): whether the reserve went since the
 * last look, which empties the model cache, and the reserve again where
 * the heap has room for it */
int ps2_heap_look(uint32_t room)
{
    int was = spent;

    spent = 0;
    if (reserve == nullptr && room >= RESERVE + RESERVE_AGAIN) {
        reserve = __real_malloc(RESERVE);
        if (reserve != nullptr) {
            fprintf(stderr, "sa1: the heap's reserve is back\n");
        }
    }
    return was;
}

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);

    return p != nullptr || !give_back(n) ? p : __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t size)
{
    void *p = __real_calloc(n, size);

    return p != nullptr || !give_back(n * size) ? p : __real_calloc(n, size);
}

void *__wrap_realloc(void *old, size_t n)
{
    void *p = __real_realloc(old, n);

    return p != nullptr || n == 0 || !give_back(n) ? p : __real_realloc(old, n);
}

void *__wrap_memalign(size_t align, size_t n)
{
    void *p = __real_memalign(align, n);

    return p != nullptr || !give_back(n) ? p : __real_memalign(align, n);
}
}
