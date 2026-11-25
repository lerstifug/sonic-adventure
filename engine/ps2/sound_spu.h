/* the PS2's sound effects on the SPU2's own voices (engine/ps2/SOUND.md's
 * stage 2), which engine/pc/mixer.h includes into pc::Sound's render on the
 * PS2.
 *
 * as the render takes a programs bank, its samples' VAG, which the build
 * made from the disc's MLT files (engine/ps2/vab.h), goes from its file to
 * a region of the SPU2's memory of the bank's own, which a bank loaded over
 * its bytes frees. a note whose sample is there plays on a voice of the
 * SPU2's: the render still moves its envelope and LFO a block at a time,
 * and sends them as the voice's volume and pitch, once a render. the SPU2's
 * own envelope stays full. the render keeps the voice's place too, by its
 * pitch, to end one that doesn't loop. a note whose sample isn't there
 * plays in the mixer, as before */
#include "pspkernel.h"
#include "spu.h"
#include "vab.h"

#include <vector>

/* the notes the SPU2's voices took, which the window's log tells */
extern "C" unsigned long ps2_spu_notes;
unsigned long ps2_spu_notes;

namespace pc {

namespace {

/* the SPU2's envelope, held full: the fastest attack, decay to the top,
 * a sustain that holds, and the fastest release, for a voice keyed off */
constexpr uint16_t SPU_ADSR1 = 0x000f, SPU_ADSR2 = 0x1fc0;
/* a piece of a bank's VAG, read and sent at a time */
constexpr uint32_t SPU_PIECE = SA1SND_CHUNK;
uint8_t spu_piece[SPU_PIECE] __attribute__((aligned(64)));

/* a gain of the mixer's as the SPU2's volume, whose 0x3fff is full */
uint16_t spu_volume(float gain)
{
    float v = gain * MASTER * 16383.0F;

    return v <= 0.0F ? 0 : v >= 16383.0F ? 16383 : static_cast<uint16_t>(v);
}

/* a step of the mixer's, at its rate, as the SPU2's pitch, whose 0x1000
 * plays at its own 48 kHz */
uint16_t spu_pitch(float step)
{
    float p = step * static_cast<float>(RATE) / 48000.0F * 4096.0F;

    return p <= 0.0F ? 0 : p >= 16383.0F ? 16383 : static_cast<uint16_t>(p);
}

} // namespace

/* the regions and samples of the banks whose bytes lay in sound memory from
 * from to to go, and the voices that played them stop */
void Sound::spu_forget(uint32_t from, uint32_t to)
{
    for (uint32_t r = 0; r < spu_region_count_;) {
        SpuRegion &region = spu_regions_[r];

        if (region.to <= from || region.from >= to) {
            r++;
            continue;
        }
        for (Voice &v : voices_) {
            if (v.on && v.hw && v.start >= region.from && v.start < region.to) {
                v.on = false;
                spu_drop(v);
            }
        }
        uint32_t kept = 0;

        for (uint32_t i = 0; i < spu_sample_count_; i++) {
            if (spu_samples_[i].start < region.from || spu_samples_[i].start >= region.to) {
                spu_samples_[kept++] = spu_samples_[i];
            }
        }
        spu_sample_count_ = kept;
        spu_bytes_ -= region.bytes;
        spu_regions_[r] = spu_regions_[--spu_region_count_];
    }
}

/* a bank the render took: what lay under its bytes goes, and a programs
 * bank's VAG goes from its file to a region of the SPU2's memory, the
 * first that has room */
void Sound::spu_bank(uint32_t address, uint32_t bytes, bool programs)
{
    spu_forget(address, address + bytes);
    if (!programs || !ps2::spu_ready() || bytes < 0x30 || spu_region_count_ >= BANKS ||
        memcmp(memory_ + address, "SMPB", 4) != 0) {
        return;
    }
    uint32_t began = sceKernelGetSystemTimeLow();
    uint32_t crc = ps2::vab_crc(memory_ + address, bytes);
    char path[40];
    ps2::VabHead head{};

    snprintf(path, sizeof path, "sound/%08x.vag", static_cast<unsigned>(crc));
    FILE *in = fopen(path, "rb");

    if (in == nullptr || fread(&head, sizeof head, 1, in) != 1 || head.magic != ps2::VAB_MAGIC ||
        spu_sample_count_ + head.samples > SPU_SAMPLES) {
        fprintf(stderr, "sa1: no VAG of a bank of %u bytes, %s: the mixer plays it\n", static_cast<unsigned>(bytes),
                path);
        if (in != nullptr) {
            fclose(in);
        }
        return;
    }
    /* the region: the first gap between the others, by their places */
    uint32_t place = SA1SND_FIRST, need = head.bytes;

    for (bool moved = true; moved;) {
        moved = false;
        for (uint32_t r = 0; r < spu_region_count_; r++) {
            const SpuRegion &o = spu_regions_[r];

            if (place < o.spu + o.bytes && o.spu < place + need) {
                place = o.spu + o.bytes;
                moved = true;
            }
        }
    }
    if (place + need > SA1SND_END) {
        fprintf(stderr, "sa1: the SPU2's memory has no room for a bank's %u bytes of VAG\n",
                static_cast<unsigned>(need));
        fclose(in);
        return;
    }
    uint32_t first = spu_sample_count_;

    for (uint32_t i = 0; i < head.samples; i++) {
        ps2::VabSample v;

        if (fread(&v, sizeof v, 1, in) != 1) {
            spu_sample_count_ = first;
            fclose(in);
            return;
        }
        SpuSample &s = spu_samples_[spu_sample_count_++];

        s.start = address + v.start;
        s.format = v.format;
        s.loops = v.loops != 0;
        s.loop_start = v.loop_start;
        s.loop_end = v.loop_end;
        s.spu = place + v.offset;
    }
    for (uint32_t done = 0; done < need;) {
        uint32_t n = need - done < SPU_PIECE ? need - done : SPU_PIECE;

        if (fread(spu_piece, 1, n, in) != n) {
            break;
        }
        ps2::spu_upload(place + done, spu_piece, n);
        done += n;
    }
    fclose(in);
    spu_regions_[spu_region_count_++] = SpuRegion{address, address + bytes, place, need};
    spu_bytes_ += need;
    spu_converted_ += head.samples;
    uint32_t took = sceKernelGetSystemTimeLow() - began;

    spu_us_ += took;
    fprintf(stderr, "sa1: a bank's %u samples went to the SPU2, %u KB of VAG in %u ms; it holds %u KB in %u banks\n",
            static_cast<unsigned>(head.samples), static_cast<unsigned>(need / 1024), static_cast<unsigned>(took / 1000),
            static_cast<unsigned>(spu_bytes_ / 1024), static_cast<unsigned>(spu_region_count_));
}

struct sa1snd_voice &Sound::spu_change(uint32_t slot)
{
    static struct sa1snd_voice spare;

    if (spu_change_[slot] == 0) {
        if (spu_frame_.count >= SA1SND_CHANGES) {
            return spare;
        }
        struct sa1snd_voice &c = spu_frame_.changes[spu_frame_.count++];

        c = sa1snd_voice{};
        c.voice = static_cast<uint16_t>(slot);
        spu_change_[slot] = static_cast<int16_t>(spu_frame_.count);
    }
    return spu_frame_.changes[spu_change_[slot] - 1];
}

/* a note just made: where its sample is in the SPU2's memory and a voice
 * there is free, that voice plays it, keyed on in this render's frame */
void Sound::spu_note(Voice &v)
{
    if (!ps2::spu_ready()) {
        return;
    }
    const SpuSample *found = nullptr;

    for (uint32_t i = 0; i < spu_sample_count_ && !found; i++) {
        const SpuSample &s = spu_samples_[i];

        if (s.start == v.start && s.format == v.format && s.loops == v.loops && s.loop_start == v.loop_start &&
            s.loop_end == v.loop_end) {
            found = &s;
        }
    }
    if (!found) {
        return;
    }
    /* core 1's voices first, which mix straight to the output */
    for (uint32_t n = 0; n < SA1SND_VOICES; n++) {
        uint32_t slot = (n + 24) % SA1SND_VOICES;

        if (spu_owner_[slot] != 0) {
            continue;
        }
        ps2_spu_notes++;
        struct sa1snd_voice &c = spu_change(slot);

        c.what |= SA1SND_SET_ENVELOPE | SA1SND_SET_START;
        c.adsr1 = SPU_ADSR1;
        c.adsr2 = SPU_ADSR2;
        c.start = found->spu;
        spu_frame_.on[slot / 24] |= 1U << (slot % 24);
        spu_owner_[slot] = static_cast<int16_t>(&v - voices_ + 1);
        v.hw = static_cast<uint8_t>(slot + 1);
        v.sent_pitch = v.sent_left = v.sent_right = 0;
        return;
    }
}

/* a voice of the SPU2's that ends, or whose note another takes, keyed off */
void Sound::spu_drop(Voice &v)
{
    if (!v.hw) {
        return;
    }
    uint32_t slot = v.hw - 1U;

    spu_frame_.off[slot / 24] |= 1U << (slot % 24);
    spu_frame_.on[slot / 24] &= ~(1U << (slot % 24));
    spu_owner_[slot] = 0;
    v.hw = 0;
}

/* a block of a voice of the SPU2's: its volume and pitch where they moved,
 * and its place, which ends a sample that doesn't loop */
void Sound::spu_block(Voice &v, float gain, float pitch, uint32_t frames)
{
    uint32_t slot = v.hw - 1U;
    uint16_t left = spu_volume(gain * v.direct_left), right = spu_volume(gain * v.direct_right);
    uint16_t p = spu_pitch(static_cast<float>(v.step) * pitch);

    if (left != v.sent_left || right != v.sent_right) {
        struct sa1snd_voice &c = spu_change(slot);

        c.what |= SA1SND_SET_VOLUME;
        c.left = left;
        c.right = right;
        v.sent_left = left;
        v.sent_right = right;
    }
    if (p != v.sent_pitch) {
        struct sa1snd_voice &c = spu_change(slot);

        c.what |= SA1SND_SET_PITCH;
        c.pitch = p;
        v.sent_pitch = p;
    }
    uint64_t moved = static_cast<uint64_t>(static_cast<float>(v.step) * pitch * 65536.0F) * frames + v.part;

    v.at += static_cast<uint32_t>(moved >> 16);
    v.part = static_cast<uint32_t>(moved & 0xffff);
    if (v.at >= v.loop_end) {
        if (!v.loops || v.loop_end <= v.loop_start) {
            v.on = false;
            spu_drop(v);
        } else {
            v.at = v.loop_start + (v.at - v.loop_start) % (v.loop_end - v.loop_start);
        }
    }
}

/* the render's frame of changes to the IOP's module, once a render */
void Sound::spu_send()
{
    if (spu_frame_.count != 0 || spu_frame_.on[0] || spu_frame_.on[1] || spu_frame_.off[0] || spu_frame_.off[1]) {
        ps2::spu_frame(spu_frame_);
    }
    spu_frame_.count = 0;
    spu_frame_.on[0] = spu_frame_.on[1] = spu_frame_.off[0] = spu_frame_.off[1] = 0;
    memset(spu_change_, 0, sizeof spu_change_);
}

} // namespace pc
