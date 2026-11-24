/* the TIM2 files of the PS2's textures (engine/ps2/TEXTURES.md): one file
 * for each name of the disc's texture files, NAME.tm2, holding every texture
 * of the files of that name, a picture each. engine/tools/textures_tm2.cpp
 * writes them on the build's host; the console reads them whole and sends
 * their texels and CLUTs to the GS as they lie.
 *
 * the layout is Sony's TIM2, at its 128-byte alignment: the file's head,
 * padded to 128 bytes, then each picture: its head, padded to a multiple of
 * 128 bytes with what follows it, then its texels, then its CLUT, each a
 * multiple of 128 bytes. a picture's head is Tim2Picture, then TIM2's
 * extended head (Tim2Extended), then the game's terms (Tim2Terms), then the
 * PVR's name as the extended head's comment.
 *
 * a picture's texels are the GS's, rows top first: 4-bit indices two to a
 * byte, the first in the low half, 8-bit indices, 16-bit texels (red's 5 bits
 * low, alpha's 1 high) or 32-bit ones (red, green, blue, alpha), each alpha
 * halved as the GS's 1.0 is 0x80. a CLUT is 32-bit colors so: 16 of them for
 * 4-bit, or 256 for 8-bit, arranged as the GS's CSM1 takes them, each run
 * of 8 from 8 to 15 traded with the run from 16 to 23 in each 32 (TIM2's
 * CLUT type's bit 7 clear). sides are powers of two of 8 to 1024, the
 * image's last row and column repeated into the rest */
#ifndef PS2_TIM2_H
#define PS2_TIM2_H

#include <stdint.h>

namespace ps2 {

constexpr uint32_t TIM2_ALIGN = 128;

struct Tim2Head {
    char magic[4];        /* "TIM2" */
    uint8_t version;      /* 4 */
    uint8_t format;       /* 1: 128-byte alignment */
    uint16_t pictures;
    uint8_t reserved[8];
};
static_assert(sizeof(Tim2Head) == 16, "TIM2's head is 16 bytes");

/* TIM2's image types and CLUT type */
constexpr uint8_t TIM2_16 = 1, TIM2_32 = 3, TIM2_4 = 4, TIM2_8 = 5;
constexpr uint8_t TIM2_CLUT_32 = 3;

struct Tim2Picture {
    uint32_t total;       /* the picture's bytes: head, texels and CLUT */
    uint32_t clut_bytes;
    uint32_t image_bytes;
    uint16_t head_bytes;
    uint16_t clut_colors;
    uint8_t picture_format; /* 0 */
    uint8_t mipmaps;        /* the levels: 1, the image alone */
    uint8_t clut_type;      /* TIM2_CLUT_32, or 0 for none */
    uint8_t image_type;     /* a TIM2_ image type */
    uint16_t width, height; /* the texels' sides, powers of two */
    uint64_t tex0, tex1;    /* the GS's TEX0 and TEX1 for it, at address 0 */
    uint32_t texa_fba_pabe;
    uint32_t tex_clut;
} __attribute__((packed));
static_assert(sizeof(Tim2Picture) == 48, "TIM2's picture head is 48 bytes");

struct Tim2Extended {
    char magic[4];        /* "eXt" */
    uint32_t user_space;  /* this head's bytes, the terms' and the comment's */
    uint32_t user_data;   /* the terms' bytes */
    uint32_t reserved;
};
static_assert(sizeof(Tim2Extended) == 16, "TIM2's extended head is 16 bytes");

/* the game's terms of a picture: the PVR's global index, which the game's
 * records find a texture by, with its name; how it was made; and the
 * image's own sides within the texels' */
constexpr uint32_t TERMS_MAGIC = 0x54314153; /* SA1T */
constexpr uint32_t TERMS_CUTS = 1;      /* each texel clear or solid but for an edge's few */
constexpr uint32_t TERMS_QUANTIZED = 2; /* its colors fit to 256: the CLUT holds the nearest */
struct Tim2Terms {
    uint32_t magic;
    uint32_t global_index;
    uint32_t flags;
    uint8_t pixel, layout;  /* the PVR's own pixel format and layout */
    uint16_t reserved;
    uint16_t width, height; /* the image's own */
    uint32_t reserved2;
};
static_assert(sizeof(Tim2Terms) == 24, "a picture's terms are 24 bytes");

/* the GS's formats, as TEX0's PSM names them */
constexpr uint8_t GS_PSM_32 = 0x00, GS_PSM_16 = 0x02, GS_PSM_8 = 0x13, GS_PSM_4 = 0x14;

/* a CLUT entry's place in the GS's CSM1 order of 256 */
inline uint32_t csm1_place(uint32_t i)
{
    return (i & ~0x18U) | (i & 0x08U) << 1 | (i & 0x10U) >> 1;
}

} // namespace ps2

#endif
