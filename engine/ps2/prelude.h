/* the PS2 slice's prelude, which the slice includes ahead of each of the
 * game's sources: the native slice's own, then the place for the console's
 * RAM the PSP's slice gives (engine/psp/psp.h), which a narrow host's copy of
 * the game's C names. the EE's N32 pointers hold 4 bytes, as the SH-4's do,
 * so the game's records lie as the console lays them, and nothing is marked */
#ifndef PS2_PRELUDE_H
#define PS2_PRELUDE_H

#include "../native/prelude.h"
#include "../psp/psp.h"

#endif
