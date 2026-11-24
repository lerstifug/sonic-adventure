/* a texture's texels as the PS2's GS takes them, made from a decoded image
 * (engine/pc/pvr.h): by the raster as a texture comes (gs.cpp), and by the
 * host ahead of time, for the pack of the disc's textures the console reads
 * whole (engine/tools/textures_pack.cpp). the two make the same bytes.
 *
 * the texels lie at power-of-two sides of 8 to 1024, the image's last row
 * and column repeated into the rest, so a clamped edge filters as the
 * image's own. at 32 bits where the image's alpha has steps between clear
 * and solid, each alpha halved as the GS's 1.0 is 0x80, else at 16 bits,
 * the alpha a bit the GS's TEXA makes clear or 0x80 */
#ifndef PS2_TEXELS_H
#define PS2_TEXELS_H

#include <stdint.h>
#include <string.h>

#include <vector>

#include "pvr.h"

namespace ps2 {

/* the GS's formats a texture lies in, as its TEX0's PSM names them */
constexpr uint8_t PSM_32 = 0x00, PSM_16 = 0x02;
constexpr uint32_t SIDE_MOST = 1024;

/* a texture's terms, as the pack holds them before its texels */
struct TexelsHead {
    uint16_t width, height; /* the image's own */
    uint8_t tw, th; /* the stored sides' log2 */
    uint8_t psm, cuts; /* a GS format, and whether the texels are each
                        * clear or solid but for an edge's few */
    uint32_t bytes; /* the texels' */
};
static_assert(sizeof(TexelsHead) == 12, "the pack's head of a texture is 12 bytes");

inline uint32_t side_log2(uint32_t n)
{
    uint32_t l = 0;

    while ((1U << l) < n) {
        l++;
    }
    return l;
}

/* image's texels, or false for an image the GS takes none of */
inline bool texels_of(const pc::Image &image, TexelsHead &head, std::vector<uint8_t> &texels)
{
    uint32_t w = image.width, h = image.height;

    if (w == 0 || h == 0 || w > SIDE_MOST || h > SIDE_MOST || image.rgba.size() < static_cast<size_t>(w) * h * 4) {
        return false;
    }
    uint32_t tw = side_log2(w < 8 ? 8 : w), th = side_log2(h < 8 ? 8 : h);
    uint32_t pw = 1U << tw, ph = 1U << th;
    uint32_t partial = 0, clear = 0;
    const uint8_t *from = image.rgba.data();

    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        uint8_t a = from[i * 4 + 3];

        partial += a != 0 && a != 0xff;
        clear += a == 0;
    }
    bool wide = partial != 0;

    head.width = static_cast<uint16_t>(w);
    head.height = static_cast<uint16_t>(h);
    head.tw = static_cast<uint8_t>(tw);
    head.th = static_cast<uint8_t>(th);
    head.psm = wide ? PSM_32 : PSM_16;
    head.cuts = clear != 0 && partial * 50 < w * h;
    head.bytes = pw * ph * (wide ? 4 : 2);
    texels.resize(head.bytes);
    for (uint32_t y = 0; y < ph; y++) {
        const uint8_t *row = from + static_cast<size_t>(y < h ? y : h - 1) * w * 4;

        for (uint32_t x = 0; x < pw; x++) {
            const uint8_t *t = row + (x < w ? x : w - 1) * 4;

            if (wide) {
                uint32_t word = t[0] | t[1] << 8 | t[2] << 16 | static_cast<uint32_t>((t[3] + 1) >> 1) << 24;

                memcpy(&texels[(static_cast<size_t>(y) * pw + x) * 4], &word, 4);
            } else {
                uint16_t half =
                    static_cast<uint16_t>(t[0] >> 3 | (t[1] >> 3) << 5 | (t[2] >> 3) << 10 | (t[3] >= 0x80 ? 0x8000 : 0));

                memcpy(&texels[(static_cast<size_t>(y) * pw + x) * 2], &half, 2);
            }
        }
    }
    return true;
}

/* the pack of the disc's textures (textures.pak): its head, then a word
 * for each place of the index of textures (textures.bin), in the index's
 * order, the byte at which its texture lies, or 0 for one the GS takes
 * none of. each texture is its TexelsHead, then its texels */
constexpr uint32_t PACK_MAGIC = 0x54325341; /* SA2T */
struct PackHead {
    uint32_t magic, places;
};

/* the index of textures, as engine/pc/pvr.cpp writes it under SA_SMALL */
constexpr uint32_t INDEX_MAGIC = 0x58544153;
struct IndexHead {
    uint32_t magic, files, places;
};
struct IndexFile {
    uint32_t id, first, count, compressed;
    char path[48];
};
struct IndexPlace {
    uint32_t global_index, at;
    char name[28];
};

} // namespace ps2

#endif
