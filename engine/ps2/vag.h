/* the SPU2's ADPCM (VAG): a block of 16 bytes holds 28 samples, its first
 * byte the predictor's number (high half) and the shift (low half), its
 * second the flags, then 14 bytes of 4-bit residuals, the first sample's
 * in the low half. the SPU2 decodes each sample as the residual shifted
 * left by 12 less the shift, plus the predictor's weighting of the last
 * two decoded, over 64. the flags: 1 ends the sample, 2 with 1 repeats from
 * the loop's start, which 4 marks (engine/ps2/SOUND.md's stage 2) */
#ifndef PS2_VAG_H
#define PS2_VAG_H

#include <stdint.h>

namespace ps2 {

constexpr uint32_t VAG_SAMPLES = 28, VAG_BYTES = 16;
constexpr uint8_t VAG_END = 1, VAG_REPEAT = 2, VAG_LOOP_START = 4;

/* the predictors' weights, over 64 */
constexpr int VAG_WEIGHTS[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

/* the encoder's state between blocks: the last two samples decoded */
struct VagState {
    int32_t s1 = 0, s2 = 0;
};

inline int32_t vag_clamp16(int32_t x)
{
    return x < -32768 ? -32768 : x > 32767 ? 32767 : x;
}

/* a block of 28 samples, as the SPU2 decodes it back: for each predictor,
 * the shift its largest residual needs, then the block encoded at that
 * shift with the decoder's own state, keeping the predictor of the least
 * error */
inline void vag_block(VagState &state, const int16_t *in, uint8_t flags, uint8_t out[VAG_BYTES])
{
    uint8_t best[VAG_BYTES] = {};
    int64_t best_error = -1;
    VagState best_state = state;

    for (int p = 0; p < 5; p++) {
        int32_t w0 = VAG_WEIGHTS[p][0], w1 = VAG_WEIGHTS[p][1];
        int32_t s1 = state.s1, s2 = state.s2, most = 0;

        /* the residuals' reach, by the input's own past */
        for (uint32_t i = 0; i < VAG_SAMPLES; i++) {
            int32_t predicted = (s1 * w0 + s2 * w1) >> 6;
            int32_t r = in[i] - predicted;

            r = r < 0 ? -r : r;
            most = r > most ? r : most;
            s2 = s1;
            s1 = in[i];
        }
        int shift = 12;

        while (shift > 0 && (most >> (12 - shift)) > 7) {
            shift--;
        }
        uint8_t block[VAG_BYTES] = {};
        int64_t error = 0;

        s1 = state.s1;
        s2 = state.s2;
        block[0] = static_cast<uint8_t>(p << 4 | shift);
        block[1] = flags;
        for (uint32_t i = 0; i < VAG_SAMPLES; i++) {
            int32_t predicted = (s1 * w0 + s2 * w1) >> 6;
            int32_t r = in[i] - predicted;
            /* the residual's nibble, rounded */
            int32_t scaled = (r * (1 << shift) + (1 << 11)) >> 12;

            scaled = scaled < -8 ? -8 : scaled > 7 ? 7 : scaled;
            int32_t decoded = vag_clamp16(((scaled << 12) >> shift) + predicted);
            int32_t e = decoded - in[i];

            error += static_cast<int64_t>(e) * e;
            block[2 + i / 2] |= static_cast<uint8_t>((scaled & 15) << (4 * (i & 1)));
            s2 = s1;
            s1 = decoded;
        }
        if (best_error < 0 || error < best_error) {
            best_error = error;
            for (uint32_t b = 0; b < VAG_BYTES; b++) {
                best[b] = block[b];
            }
            best_state.s1 = s1;
            best_state.s2 = s2;
        }
    }
    for (uint32_t b = 0; b < VAG_BYTES; b++) {
        out[b] = best[b];
    }
    state = best_state;
}

} // namespace ps2

#endif
