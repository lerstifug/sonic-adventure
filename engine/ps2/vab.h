/* a programs bank's samples as the SPU2 plays them (engine/ps2/SOUND.md's
 * stage 2), made once on the build's host (engine/tools/sounds_vag.cpp)
 * from the disc's MLT files, and read by the PS2 as the game loads the bank
 * (engine/ps2/sound_spu.h). one file a bank, game/sound/CRC.vag, by the
 * CRC32 of the bank's bytes, which the game takes whole from its file:
 *
 *   VabHead, then a VabSample for each of the bank's samples, then the
 *   samples' VAG, each at its offset from the VAG's start, a multiple of 64
 *
 * a sample is each split's, once: where it lies in the bank, its format and
 * its loop, as the AICA reads them (engine/pc/sound.cpp's note_on).
 *
 * the SPU2 loops from the start of a block of 28 samples, and the length a
 * loop is held to is a whole count of blocks. so the sample starts later by
 * the samples that bring its loop's start to a block's, and its loop is
 * repeated till it's a whole count of blocks. a loop that would take too
 * many repeats is stretched to the nearest whole count instead, which moves
 * its pitch by under a block's share of its length */
#ifndef PS2_VAB_H
#define PS2_VAB_H

#include <stdint.h>
#include <string.h>

#include <vector>

#include "vag.h"

namespace ps2 {

constexpr uint32_t VAB_MAGIC = 0x56314153; /* SA1V */
constexpr uint32_t VAB_ALIGN = 64;
/* the most samples a loop's repeats take before it's stretched instead */
constexpr uint32_t VAB_REPEATS_MOST = 8192;

struct VabHead {
    uint32_t magic;
    uint32_t samples;
    uint32_t bytes; /* the VAG's, after the table */
    uint32_t reserved;
};
static_assert(sizeof(VabHead) == 16, "a VAB's head is 16 bytes");

struct VabSample {
    uint32_t start; /* from the bank's start */
    uint16_t format, loops;
    uint32_t loop_start, loop_end; /* in samples */
    uint32_t offset;               /* its VAG's, from the VAG's start */
};
static_assert(sizeof(VabSample) == 20, "a VAB's sample is 20 bytes");

/* the AICA's formats, and its ADPCM, as engine/pc/sound.cpp decodes them */
enum { AICA_PCM16 = 0, AICA_PCM8 = 1, AICA_ADPCM = 2 };
constexpr int AICA_SIZE[8] = {1, 3, 5, 7, 9, 11, 13, 15};
constexpr int AICA_SCALE[8] = {230, 230, 230, 230, 307, 409, 512, 614};
constexpr int AICA_LEAST = 0x7f, AICA_MOST = 0x6000;
constexpr uint32_t AICA_SPLIT = 0x30;

inline uint32_t vab_read16(const uint8_t *at)
{
    return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8;
}

inline uint32_t vab_read32(const uint8_t *at)
{
    return vab_read16(at) | vab_read16(at + 2) << 16;
}

/* bank + offset where count records of each bytes fit in size, else null */
inline const uint8_t *vab_within(const uint8_t *bank, uint32_t size, uint32_t offset, uint32_t count, uint32_t each)
{
    return offset <= size && count <= (size - offset) / each ? bank + offset : nullptr;
}

/* each split's sample of a programs bank, once */
inline std::vector<VabSample> vab_samples(const uint8_t *bank, uint32_t bytes)
{
    std::vector<VabSample> found;

    if (bytes < 0x30 || memcmp(bank, "SMPB", 4) != 0) {
        return found;
    }
    uint32_t count = vab_read32(bank + 0x14);
    const uint8_t *table = count ? vab_within(bank, bytes, vab_read32(bank + 0x10), count, 4) : nullptr;

    for (uint32_t p = 0; table && p < count; p++) {
        uint32_t program = vab_read32(table + 4 * p);
        const uint8_t *layers = program ? vab_within(bank, bytes, program, 4, 4) : nullptr;

        for (uint32_t layer = 0; layers && layer < 4; layer++) {
            uint32_t at = vab_read32(layers + 4 * layer);
            const uint8_t *head = at ? vab_within(bank, bytes, at, 1, 16) : nullptr;

            if (!head) {
                continue;
            }
            uint32_t splits = vab_read32(head), first = vab_read32(head + 4);
            uint32_t held = first <= bytes ? (bytes - first) / AICA_SPLIT : 0;

            for (uint32_t s = 0; s < splits && s < held; s++) {
                const uint8_t *split = bank + first + AICA_SPLIT * s;
                uint32_t r00 = vab_read16(split), r04 = vab_read16(split + 2);
                VabSample sample{};

                sample.start = (r00 & 0x7f) << 16 | r04;
                sample.format = static_cast<uint16_t>(r00 >> 7 & 3);
                sample.loops = static_cast<uint16_t>(r00 >> 9 & 1);
                sample.loop_start = vab_read16(split + 4);
                sample.loop_end = vab_read16(split + 6);
                bool known = sample.start >= bytes || sample.format > AICA_ADPCM || sample.loop_end == 0;

                for (const VabSample &o : found) {
                    known = known || (o.start == sample.start && o.format == sample.format && o.loops == sample.loops &&
                                      o.loop_start == sample.loop_start && o.loop_end == sample.loop_end);
                }
                if (!known) {
                    found.push_back(sample);
                }
            }
        }
    }
    return found;
}

/* how a sample lies as VAG: the samples before its start that bring its
 * loop's start to a block's, its loop's length as played, whether that's
 * stretched, and the samples in all, a whole count of blocks */
struct VabLayout {
    uint32_t lead, loop, total;
    bool stretched;
};

inline VabLayout vab_layout(const VabSample &s)
{
    VabLayout l{};

    if (!s.loops || s.loop_end <= s.loop_start) {
        l.total = (s.loop_end + VAG_SAMPLES - 1) / VAG_SAMPLES * VAG_SAMPLES;
        return l;
    }
    uint32_t length = s.loop_end - s.loop_start, a = length, b = VAG_SAMPLES;

    while (b != 0) {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    uint32_t repeats = VAG_SAMPLES / a;

    l.lead = (VAG_SAMPLES - s.loop_start % VAG_SAMPLES) % VAG_SAMPLES;
    if (length * repeats <= VAB_REPEATS_MOST) {
        l.loop = length * repeats;
    } else {
        uint32_t blocks = (length + VAG_SAMPLES / 2) / VAG_SAMPLES;

        l.loop = (blocks ? blocks : 1) * VAG_SAMPLES;
        l.stretched = true;
    }
    l.total = l.lead + s.loop_start + l.loop;
    return l;
}

inline uint32_t vab_bytes(const VabLayout &l)
{
    uint32_t bytes = l.total / VAG_SAMPLES * VAG_BYTES;

    return (bytes + VAB_ALIGN - 1) & ~(VAB_ALIGN - 1);
}

/* a sample's own samples, from the bank's bytes, as the AICA decodes them */
inline std::vector<int16_t> vab_decode(const uint8_t *bank, uint32_t bytes, const VabSample &s)
{
    std::vector<int16_t> out(s.loop_end, 0);
    int32_t value = 0, quant = AICA_LEAST;

    for (uint32_t i = 0; i < s.loop_end; i++) {
        uint32_t at = s.start + (s.format == AICA_PCM16 ? 2 * i : s.format == AICA_PCM8 ? i : i / 2);

        if (at + (s.format == AICA_PCM16 ? 2 : 1) > bytes) {
            break;
        }
        if (s.format == AICA_PCM16) {
            out[i] = static_cast<int16_t>(vab_read16(bank + at));
        } else if (s.format == AICA_PCM8) {
            out[i] = static_cast<int16_t>(static_cast<int8_t>(bank[at]) * 256);
        } else {
            uint32_t code = bank[at] >> (i & 1 ? 4 : 0) & 15;
            int32_t change = quant * AICA_SIZE[code & 7] / 8;

            change = change > 0x7fff ? 0x7fff : change;
            value = vag_clamp16(value + (code & 8 ? -change : change));
            quant = quant * AICA_SCALE[code & 7] >> 8;
            quant = quant < AICA_LEAST ? AICA_LEAST : quant > AICA_MOST ? AICA_MOST : quant;
            out[i] = static_cast<int16_t>(value);
        }
    }
    return out;
}

/* a sample's VAG, laid out as vab_layout lays it, vab_bytes long */
inline std::vector<uint8_t> vab_encode(const uint8_t *bank, uint32_t bytes, const VabSample &s)
{
    VabLayout l = vab_layout(s);
    std::vector<int16_t> own = vab_decode(bank, bytes, s), pcm(l.total, 0);
    std::vector<uint8_t> out(vab_bytes(l), 0);

    if (l.loop == 0) {
        memcpy(pcm.data(), own.data(), own.size() * sizeof own[0]);
    } else {
        uint32_t length = s.loop_end - s.loop_start;
        int16_t *loop = pcm.data() + l.lead + s.loop_start;

        memcpy(pcm.data() + l.lead, own.data(), s.loop_start * sizeof own[0]);
        for (uint32_t i = 0; i < l.loop; i++) {
            if (!l.stretched) {
                loop[i] = own[s.loop_start + i % length];
                continue;
            }
            /* between the two samples around its place, the loop's end
             * running on to its start */
            double at = static_cast<double>(i) * length / l.loop;
            uint32_t a = static_cast<uint32_t>(at);
            double part = at - a;
            int32_t x = own[s.loop_start + a % length], y = own[s.loop_start + (a + 1) % length];

            loop[i] = static_cast<int16_t>(x + (y - x) * part);
        }
    }
    uint32_t blocks = l.total / VAG_SAMPLES;
    uint32_t loop_block = l.loop ? (l.lead + s.loop_start) / VAG_SAMPLES : blocks;
    VagState state;

    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t flags = 0;

        if (b == loop_block) {
            flags |= VAG_LOOP_START;
        }
        if (b + 1 == blocks) {
            flags |= l.loop ? VAG_END | VAG_REPEAT : VAG_END;
        }
        vag_block(state, pcm.data() + b * VAG_SAMPLES, flags, out.data() + b * VAG_BYTES);
    }
    return out;
}

/* the CRC32 (IEEE's, reflected) of bytes, a bank's name */
inline uint32_t vab_crc(const uint8_t *bytes, uint32_t count)
{
    static uint32_t table[256];
    static bool made;

    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xedb88320U ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xffffffffU;

    for (uint32_t i = 0; i < count; i++) {
        c = table[(c ^ bytes[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffffU;
}

} // namespace ps2

#endif
