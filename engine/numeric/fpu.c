/* play math by the host's own FPU, for a host too slow for fast.c's
 * integers: the PSP and the PS2, whose processors take some hundred
 * instructions for a sum of four products made as the SH-4 makes it, where
 * the game makes thousands of them a picture. FIPR and FTRV are the FPU's
 * own here, each in its operation's order, under the mode its caller holds,
 * as the game's adds and multiplies are on every host. so their bits are
 * that FPU's: not math.c's, and not another host's.
 *
 * FMAC and FSRRA stay fast.c's, the console's bits: they cost little, and
 * the FPU's differ where the game's collision tells a side of a polygon by
 * them. by the FPU's, Emerald Coast caught the player at a seam of its
 * ground, near -855 46 1560, until he jumped, where the console's let him
 * by; exact FIPR and FTRV alone didn't, and doubled a picture's time. the
 * rest are fast.c's too, which this file holds under fast_ names for the
 * four */
#include "play_math.h"

#define SA_NUMERIC_FPU
#include "fast.c"
#undef sa_fmac
#undef sa_fipr
#undef sa_ftrv
#undef sa_fsrra

/* the SH-4's FMAC rounds once, where the FPU's multiply and add round
 * twice */
float sa_fmac(float f0, float fm, float fn)
{
    return fast_fmac(f0, fm, fn);
}

float sa_fipr(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

void sa_ftrv(const float m[16], const float v[4], float out[4])
{
    float sums[4];
    int i;

    for (i = 0; i < 4; i++) {
        sums[i] =
            m[i] * v[0] + m[i + 4] * v[1] + m[i + 8] * v[2] + m[i + 12] * v[3];
    }
    memcpy(out, sums, sizeof sums);
}

/* the SH-4's FSRRA is its own approximation, which a divide by the FPU's
 * square root doesn't give */
float sa_fsrra(float a)
{
    return fast_fsrra(a);
}
