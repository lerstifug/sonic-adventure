/* the PS2's sound effects on the SPU2's own voices, the EE's half
 * (engine/ps2/spu.cpp, engine/ps2/SOUND.md's stage 2) */
#ifndef PS2_SPU_H
#define PS2_SPU_H

#include <stddef.h>
#include <stdint.h>

#include "sa1snd.h"

namespace ps2 {

/* load the IOP's module and bind its server, after audsrv has started the
 * SPU2's driver. false, with why, where it can't */
bool spu_open(const char *&why);
/* count bytes to the SPU2's memory at address, a multiple of 64 */
void spu_upload(uint32_t address, const void *bytes, uint32_t count);
/* whether the module answers */
bool spu_ready();
/* a frame of the voices' changes and keys */
void spu_frame(const struct sa1snd_frame &frame);
/* the voices' envelopes and ends, as they stand */
bool spu_status(struct sa1snd_status &status);

} // namespace ps2

#endif
