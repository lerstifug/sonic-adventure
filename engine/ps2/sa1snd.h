/* the PS2's sound effects on the SPU2's own voices (engine/ps2/SOUND.md's
 * stage 2): what the EE (engine/ps2/spu.cpp) and the IOP's module
 * (engine/ps2/iop/sa1snd.c) say to each other by SIF RPC. C, for both */
#ifndef PS2_SA1SND_H
#define PS2_SA1SND_H

#include <stdint.h>

#define SA1SND_SERVER 0x5a150001
/* the calls */
#define SA1SND_PING 0   /* answers SA1SND_MAGIC in its first word */
#define SA1SND_UPLOAD 1 /* struct sa1snd_upload: bytes to the SPU2's memory */
#define SA1SND_FRAME 2  /* struct sa1snd_frame: the voices' changes */
#define SA1SND_STATUS 3 /* answers struct sa1snd_status */
#define SA1SND_EFFECT 4 /* struct sa1snd_effect: the cores' reverb */
#define SA1SND_MAGIC 0x53413153 /* SA1S */

/* the most an upload carries, and its head's bytes, which keep the bytes
 * after it on the 64 the SPU2's DMA takes */
#define SA1SND_CHUNK (16 * 1024)
#define SA1SND_HEAD 64
/* the voices: 24 a core, the second core's from 24 */
#define SA1SND_VOICES 48
/* where the SPU2's memory is the effects' from: past the cores' own
 * areas at its start, and up to the reverb's work areas at its end, core
 * 0's to SA1SND_EFFECT_END0 and core 1's to SA1SND_EFFECT_END1, each under
 * 128 KB */
#define SA1SND_FIRST 0x6000U
#define SA1SND_END 0x1c0000U
#define SA1SND_EFFECT_END0 0x1dffffU
#define SA1SND_EFFECT_END1 0x1fffffU

struct sa1snd_upload {
    uint32_t address, bytes; /* in the SPU2's memory, a multiple of 64 */
    uint32_t reserved[SA1SND_HEAD / 4 - 2];
    uint8_t data[SA1SND_CHUNK];
};

/* a voice's change. what says which fields count */
#define SA1SND_SET_PITCH 1
#define SA1SND_SET_ENVELOPE 2 /* adsr1, adsr2 */
#define SA1SND_SET_VOLUME 4   /* left, right */
#define SA1SND_SET_START 8    /* start: its sample, which holds its loop */
#define SA1SND_SET_WET 16     /* wet: to its core's reverb alone, or dry alone */
struct sa1snd_voice {
    uint16_t voice, what;
    uint16_t pitch, adsr1, adsr2;
    uint16_t left, right, wet;
    uint32_t start;
};
/* a frame's changes, then the voices keyed off and on, a word a core */
#define SA1SND_CHANGES 96
struct sa1snd_frame {
    uint32_t count;
    uint32_t off[2], on[2];
    uint32_t reserved;
    struct sa1snd_voice changes[SA1SND_CHANGES];
};
/* the cores' reverb: its mode, one of libsd's SD_EFFECT_MODE_, 0 for none,
 * and its return's depth, of 0x7fff. a call with the mode as it stands
 * moves the depth alone */
struct sa1snd_effect {
    uint32_t mode;
    int32_t depth;
    int32_t delay, feedback;
};
/* each voice's envelope level, and the voices that reached their end */
struct sa1snd_status {
    uint16_t envelope[SA1SND_VOICES];
    uint32_t ended[2];
};

#endif
