/* the one call of the PSP's kernel that the PSP's scene (engine/psp/direct.cpp)
 * makes, which the PS2's slice compiles as its own, and the PS2's raster and
 * walk time themselves by: the time in microseconds. engine/mk/ps2.mk puts
 * engine/ps2/ ahead of the system's headers, so this header is the one that
 * source finds.
 *
 * the EE counts its cycles in COP0's Count, at 294.912 MHz, which one
 * instruction reads: a call of the C library's clock takes far longer, and
 * the walk times each model. Count wraps each 14.5 seconds, so a running
 * total of its steps, one to each source that includes this, gives the
 * microseconds, where the source calls at least once in that time */
#ifndef PS2_PSPKERNEL_H
#define PS2_PSPKERNEL_H

#include <stdint.h>

static inline uint32_t sceKernelGetSystemTimeLow(void)
{
    static uint32_t last;
    static uint64_t cycles;
    uint32_t count;

    __asm__ volatile("mfc0 %0, $9" : "=r"(count));
    cycles += (uint32_t)(count - last);
    last = count;
    return (uint32_t)(cycles / 295U);
}

#endif
