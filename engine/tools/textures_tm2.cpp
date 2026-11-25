/* textures_tm2 DISC DIR writes DIR/textures/NAME.tm2 for each name of the
 * disc's texture files, and DIR/textures/report.csv, on the build's host
 * (engine/ps2/TEXTURES.md, engine/ps2/tim2.h).
 *
 * the disc's textures are those its index finds (engine/pc/pvr.cpp's, as
 * textures_pack reads it): the PVMs, PRS-compressed or not, and the loose
 * PVRs. a name's files go to one TIM2, a picture for each texture, one
 * texture that two of them hold alike once.
 *
 * each picture takes the smallest of the GS's formats that holds it whole:
 * 4-bit indices with a CLUT of 16 where it has as few colors, 8-bit with a
 * CLUT of 256 where it has as few, else 16 bits where its alpha is clear or
 * solid, else 32. a VQ texture of more colors, at most its book's 1024, is
 * fit to 256 where the fit's error is below FIT_PSNR, and so 8-bit too.
 * each picture is decoded back and checked against its image: a whole one
 * may differ by the alpha's halving alone, and the 16-bit by the bits the
 * GS's 5 drop.
 *
 * a file is written only where its bytes changed, so a run folder's copy
 * takes only those. report.csv says, a row each, what each texture became
 * and how far its texels lie from its image */
#include <math.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "disc.h"
#include "pvr.h"
#include "texels.h"
#include "tim2.h"

namespace {

/* a fit to 256 colors stands where its error is this low, in dB */
constexpr double FIT_PSNR = 40.0;
/* the PVR's VQ layouts */
bool is_vq(uint8_t layout)
{
    return layout == 3 || layout == 4 || layout == 16 || layout == 17;
}

bool read_all(const std::string &path, std::vector<uint8_t> &bytes)
{
    FILE *in = fopen(path.c_str(), "rb");
    uint8_t block[1 << 16];
    size_t got;

    if (in == nullptr) {
        return false;
    }
    bytes.clear();
    while ((got = fread(block, 1, sizeof block, in)) != 0) {
        bytes.insert(bytes.end(), block, block + got);
    }
    fclose(in);
    return true;
}

void put(std::vector<uint8_t> &out, const void *data, size_t bytes)
{
    const uint8_t *from = static_cast<const uint8_t *>(data);

    out.insert(out.end(), from, from + bytes);
}

void pad(std::vector<uint8_t> &out, size_t to)
{
    while (out.size() % to != 0) {
        out.push_back(0);
    }
}

/* a texture as the disc holds it */
struct Source {
    std::string path; /* its file's */
    uint32_t global_index = 0;
    std::string name;
    uint8_t pixel = 0, layout = 0;
    pc::Image image;
};

/* a picture made of it */
struct Made {
    ps2::Tim2Picture head{};
    std::vector<uint8_t> texels, clut;
    uint32_t flags = 0;
    const char *kind = "";
    uint32_t colors = 0;
    double psnr = INFINITY;
    int most = 0; /* the largest difference of a channel */
    /* the mip levels past the first, the largest first, as the GS takes
     * them, and each one's TBW */
    std::vector<std::vector<uint8_t>> levels;
    std::vector<uint32_t> level_tbw;
};

uint32_t rgba_at(const pc::Image &image, uint32_t x, uint32_t y)
{
    uint32_t c;

    x = x < image.width ? x : image.width - 1;
    y = y < image.height ? y : image.height - 1;
    memcpy(&c, &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4], 4);
    return c;
}

uint8_t channel(uint32_t c, int k)
{
    return static_cast<uint8_t>(c >> (8 * k));
}

/* a color as a CLUT entry holds it: the alpha halved, as texels_of halves */
uint32_t gs_color(uint32_t c)
{
    return (c & 0xffffffU) | static_cast<uint32_t>((channel(c, 3) + 1) >> 1) << 24;
}

/* and back: the alpha the GS's halved one stands for */
uint32_t from_gs(uint32_t c)
{
    uint32_t a = channel(c, 3);

    return (c & 0xffffffU) | (a >= 0x80 ? 255U : a * 2) << 24;
}

int distance(uint32_t a, uint32_t b)
{
    int d = 0;

    for (int k = 0; k < 4; k++) {
        int e = static_cast<int>(channel(a, k)) - static_cast<int>(channel(b, k));

        d += e * e;
    }
    return d;
}

/* a fit of colors, each with its count of texels, to at most 256: the box
 * of the most texels times its widest side splits at its weighted middle
 * along that side, until 256 boxes, and then each color goes to its
 * nearest box's mean and each mean to its colors' mean, a few times */
std::vector<uint32_t> fit(const std::vector<std::pair<uint32_t, uint32_t>> &colors)
{
    struct Box {
        std::vector<size_t> members;
    };
    std::vector<Box> boxes(1);

    for (size_t i = 0; i < colors.size(); i++) {
        boxes[0].members.push_back(i);
    }
    while (boxes.size() < 256) {
        size_t pick = boxes.size();
        int pick_side = 0;
        double pick_score = 0.0;

        for (size_t b = 0; b < boxes.size(); b++) {
            if (boxes[b].members.size() < 2) {
                continue;
            }
            int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
            double weight = 0.0;

            for (size_t m : boxes[b].members) {
                for (int k = 0; k < 4; k++) {
                    lo[k] = std::min<int>(lo[k], channel(colors[m].first, k));
                    hi[k] = std::max<int>(hi[k], channel(colors[m].first, k));
                }
                weight += colors[m].second;
            }
            int side = 0;

            for (int k = 1; k < 4; k++) {
                side = hi[k] - lo[k] > hi[side] - lo[side] ? k : side;
            }
            double score = weight * (hi[side] - lo[side]);

            if (hi[side] > lo[side] && score > pick_score) {
                pick = b;
                pick_side = side;
                pick_score = score;
            }
        }
        if (pick == boxes.size()) {
            break;
        }
        std::vector<size_t> &m = boxes[pick].members;

        std::sort(m.begin(), m.end(), [&](size_t a, size_t b) {
            return channel(colors[a].first, pick_side) < channel(colors[b].first, pick_side);
        });
        double total = 0.0, half = 0.0;

        for (size_t i : m) {
            total += colors[i].second;
        }
        size_t cut = 1;

        for (; cut < m.size() - 1; cut++) {
            half += colors[m[cut - 1]].second;
            if (half * 2.0 >= total) {
                break;
            }
        }
        Box other;

        other.members.assign(m.begin() + static_cast<long>(cut), m.end());
        m.resize(cut);
        boxes.push_back(other);
    }
    std::vector<uint32_t> means(boxes.size());

    for (int pass = 0; pass <= 6; pass++) {
        /* each box's mean */
        for (size_t b = 0; b < boxes.size(); b++) {
            double sum[4] = {0, 0, 0, 0}, weight = 0.0;

            for (size_t i : boxes[b].members) {
                for (int k = 0; k < 4; k++) {
                    sum[k] += channel(colors[i].first, k) * static_cast<double>(colors[i].second);
                }
                weight += colors[i].second;
            }
            uint32_t mean = 0;

            for (int k = 0; k < 4 && weight > 0.0; k++) {
                mean |= static_cast<uint32_t>(lround(sum[k] / weight)) << (8 * k);
            }
            means[b] = mean;
        }
        if (pass == 6) {
            break;
        }
        /* each color to its nearest mean */
        for (Box &box : boxes) {
            box.members.clear();
        }
        for (size_t i = 0; i < colors.size(); i++) {
            size_t best = 0;
            int best_d = INT32_MAX;

            for (size_t b = 0; b < means.size(); b++) {
                int d = distance(colors[i].first, means[b]);

                if (d < best_d) {
                    best_d = d;
                    best = b;
                }
            }
            boxes[best].members.push_back(i);
        }
    }
    return means;
}

/* the PVR's layouts that carry mip levels: the GS's mipmapping stands in
 * for them */
bool has_mips(uint8_t layout)
{
    return layout == 0x02 || layout == 0x04 || layout == 0x11 || layout == 0x12;
}

/* a TBW for a level of w texels' width in psm */
uint32_t tbw_for(uint8_t psm, uint32_t w)
{
    uint32_t tbw = (w + 63) / 64;

    if (psm == ps2::GS_PSM_8 || psm == ps2::GS_PSM_4) {
        tbw = std::max<uint32_t>(2, (tbw + 1) & ~1U);
    }
    return tbw;
}

/* the level below rgba, of w by h: each texel the mean of four, its color
 * weighted by their alpha so a clear texel's color darkens no edge */
std::vector<uint32_t> half_of(const std::vector<uint32_t> &rgba, uint32_t w, uint32_t h)
{
    uint32_t hw = w / 2, hh = h / 2;
    std::vector<uint32_t> out(static_cast<size_t>(hw) * hh);

    for (uint32_t y = 0; y < hh; y++) {
        for (uint32_t x = 0; x < hw; x++) {
            uint32_t four[4] = {rgba[(2 * y) * w + 2 * x], rgba[(2 * y) * w + 2 * x + 1],
                                rgba[(2 * y + 1) * w + 2 * x], rgba[(2 * y + 1) * w + 2 * x + 1]};
            double sum[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, alpha = 0.0;

            for (uint32_t c : four) {
                double a = channel(c, 3);

                for (int k = 0; k < 3; k++) {
                    sum[k] += channel(c, k) * a;
                    plain[k] += channel(c, k);
                }
                alpha += a;
            }
            uint32_t mean = static_cast<uint32_t>(lround(alpha / 4.0)) << 24;

            for (int k = 0; k < 3; k++) {
                mean |= static_cast<uint32_t>(lround(alpha > 0.0 ? sum[k] / alpha : plain[k] / 4.0)) << (8 * k);
            }
            out[static_cast<size_t>(y) * hw + x] = mean;
        }
    }
    return out;
}

/* the TEX0 of a picture at address 0, its CLUT at 0 */
uint64_t tex0_of(uint8_t psm, uint32_t w, uint32_t h, bool clut)
{
    uint32_t tbw = (w + 63) / 64;

    if (psm == ps2::GS_PSM_8 || psm == ps2::GS_PSM_4) {
        tbw = std::max<uint32_t>(2, (tbw + 1) & ~1U);
    }
    return static_cast<uint64_t>(tbw) << 14 | static_cast<uint64_t>(psm) << 20 |
           static_cast<uint64_t>(ps2::side_log2(w)) << 26 | static_cast<uint64_t>(ps2::side_log2(h)) << 30 |
           static_cast<uint64_t>(1) << 34 | (clut ? static_cast<uint64_t>(1) << 61 : 0);
}

/* the picture of an image, by the rules above */
Made make(const Source &s)
{
    const pc::Image &image = s.image;
    uint32_t pw = 1U << ps2::side_log2(image.width < 8 ? 8 : image.width);
    uint32_t ph = 1U << ps2::side_log2(image.height < 8 ? 8 : image.height);
    std::unordered_map<uint32_t, uint32_t> counts;
    Made made;
    ps2::TexelsHead th{};
    std::vector<uint8_t> flat;

    for (uint32_t i = 0; i < image.width * image.height; i++) {
        uint32_t c;

        memcpy(&c, &image.rgba[static_cast<size_t>(i) * 4], 4);
        counts[c]++;
    }
    made.colors = static_cast<uint32_t>(counts.size());
    ps2::texels_of(image, th, flat);
    made.flags = th.cuts ? ps2::TERMS_CUTS : 0;

    /* the palette, where one holds it */
    std::vector<uint32_t> palette;
    bool four = counts.size() <= 16 && pw >= 16;

    if (counts.size() <= 256 && pw >= 8) {
        for (const auto &kv : counts) {
            palette.push_back(kv.first);
        }
        std::sort(palette.begin(), palette.end());
        made.kind = four ? "4-bit" : "8-bit";
    } else if (is_vq(s.layout) && pw >= 8) {
        std::vector<std::pair<uint32_t, uint32_t>> colors(counts.begin(), counts.end());

        std::sort(colors.begin(), colors.end());
        palette = fit(colors);
        made.kind = "8-bit fit";
        made.flags |= ps2::TERMS_QUANTIZED;
    }
    std::vector<uint32_t> back(static_cast<size_t>(pw) * ph);

    if (!palette.empty()) {
        std::unordered_map<uint32_t, uint8_t> index;
        uint32_t entries = four ? 16 : 256;

        for (const auto &kv : counts) {
            size_t best = 0;
            int best_d = INT32_MAX;

            for (size_t p = 0; p < palette.size(); p++) {
                int d = distance(kv.first, palette[p]);

                if (d < best_d) {
                    best_d = d;
                    best = p;
                }
            }
            index[kv.first] = static_cast<uint8_t>(best);
        }
        made.texels.assign(four ? static_cast<size_t>(pw) * ph / 2 : static_cast<size_t>(pw) * ph, 0);
        for (uint32_t y = 0; y < ph; y++) {
            for (uint32_t x = 0; x < pw; x++) {
                uint8_t i = index[rgba_at(image, x, y)];
                size_t at = static_cast<size_t>(y) * pw + x;

                if (four) {
                    made.texels[at / 2] |= static_cast<uint8_t>(i << (4 * (at & 1)));
                } else {
                    made.texels[at] = i;
                }
                back[at] = from_gs(gs_color(palette[i]));
            }
        }
        made.clut.assign(entries * 4, 0);
        for (uint32_t i = 0; i < palette.size(); i++) {
            uint32_t c = gs_color(palette[i]);

            memcpy(&made.clut[(four ? i : ps2::csm1_place(i)) * 4], &c, 4);
        }
        made.head.clut_colors = static_cast<uint16_t>(entries);
        made.head.clut_type = ps2::TIM2_CLUT_32;
        made.head.image_type = four ? ps2::TIM2_4 : ps2::TIM2_8;
        made.head.tex0 = tex0_of(four ? ps2::GS_PSM_4 : ps2::GS_PSM_8, pw, ph, true);
    } else {
        bool wide = th.psm == ps2::PSM_32;

        made.texels = flat;
        made.kind = wide ? "32-bit" : "16-bit";
        for (size_t at = 0; at < back.size(); at++) {
            if (wide) {
                uint32_t c;

                memcpy(&c, &flat[at * 4], 4);
                back[at] = from_gs(c);
            } else {
                uint16_t t;

                memcpy(&t, &flat[at * 2], 2);
                back[at] = (t & 31U) << 3 | (t >> 5 & 31U) << 11 | (t >> 10 & 31U) << 19 |
                           ((t & 0x8000U) != 0 ? 0xff000000U : 0);
            }
        }
        made.head.image_type = wide ? ps2::TIM2_32 : ps2::TIM2_16;
        made.head.tex0 = tex0_of(wide ? ps2::GS_PSM_32 : ps2::GS_PSM_16, pw, ph, false);
    }
    /* the texels decoded back, against the image */
    double squares = 0.0;

    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            uint32_t a = rgba_at(image, x, y), b = back[static_cast<size_t>(y) * pw + x];

            for (int k = 0; k < 4; k++) {
                int e = static_cast<int>(channel(a, k)) - static_cast<int>(channel(b, k));

                squares += e * e;
                made.most = std::max(made.most, abs(e));
            }
        }
    }
    double mse = squares / (4.0 * image.width * image.height);

    made.psnr = mse > 0.0 ? 10.0 * log10(255.0 * 255.0 / mse) : INFINITY;
    made.head.width = static_cast<uint16_t>(pw);
    made.head.height = static_cast<uint16_t>(ph);
    made.head.mipmaps = 1;
    /* the mip levels, where the PVR had them: each made from the one above,
     * at the top's format, a CLUT's indices to its nearest entries, down to
     * sides of 8, or 16 wide at 4 bits, which the GS's transfers take whole */
    if (has_mips(s.layout)) {
        uint8_t psm = static_cast<uint8_t>(made.head.tex0 >> 20 & 0x3f);
        std::vector<uint32_t> level(static_cast<size_t>(pw) * ph);
        std::unordered_map<uint32_t, uint8_t> nearest;
        uint32_t lw = pw, lh = ph;

        for (uint32_t y = 0; y < ph; y++) {
            for (uint32_t x = 0; x < pw; x++) {
                level[static_cast<size_t>(y) * pw + x] = rgba_at(image, x, y);
            }
        }
        while (made.levels.size() + 1 < ps2::LEVELS_MOST && lw / 2 >= (psm == ps2::GS_PSM_4 ? 16U : 8U) &&
               lh / 2 >= 8) {
            level = half_of(level, lw, lh);
            lw /= 2;
            lh /= 2;
            std::vector<uint8_t> bytes;

            if (!palette.empty()) {
                bytes.assign(psm == ps2::GS_PSM_4 ? level.size() / 2 : level.size(), 0);
                for (size_t at = 0; at < level.size(); at++) {
                    auto found = nearest.find(level[at]);
                    uint8_t i;

                    if (found != nearest.end()) {
                        i = found->second;
                    } else {
                        size_t best = 0;
                        int best_d = INT32_MAX;

                        for (size_t q = 0; q < palette.size(); q++) {
                            int d = distance(level[at], palette[q]);

                            if (d < best_d) {
                                best_d = d;
                                best = q;
                            }
                        }
                        i = static_cast<uint8_t>(best);
                        nearest[level[at]] = i;
                    }
                    if (psm == ps2::GS_PSM_4) {
                        bytes[at / 2] |= static_cast<uint8_t>(i << (4 * (at & 1)));
                    } else {
                        bytes[at] = i;
                    }
                }
            } else if (psm == ps2::GS_PSM_32) {
                bytes.resize(level.size() * 4);
                for (size_t at = 0; at < level.size(); at++) {
                    uint32_t c = gs_color(level[at]);

                    memcpy(&bytes[at * 4], &c, 4);
                }
            } else {
                bytes.resize(level.size() * 2);
                for (size_t at = 0; at < level.size(); at++) {
                    uint32_t c = level[at];
                    uint16_t half = static_cast<uint16_t>(channel(c, 0) >> 3 | (channel(c, 1) >> 3) << 5 |
                                                          (channel(c, 2) >> 3) << 10 |
                                                          (channel(c, 3) >= 0x80 ? 0x8000 : 0));

                    memcpy(&bytes[at * 2], &half, 2);
                }
            }
            made.levels.push_back(bytes);
            made.level_tbw.push_back(tbw_for(psm, lw));
        }
        made.head.mipmaps = static_cast<uint8_t>(1 + made.levels.size());
    }
    return made;
}

/* the TIM2 of a name's pictures */
std::vector<uint8_t> tim2_of(const std::vector<Source> &sources, const std::vector<Made> &made)
{
    std::vector<uint8_t> out;
    ps2::Tim2Head head = {{'T', 'I', 'M', '2'}, 4, 1, static_cast<uint16_t>(made.size()), {}};

    put(out, &head, sizeof head);
    pad(out, ps2::TIM2_ALIGN);
    for (size_t i = 0; i < made.size(); i++) {
        const Source &s = sources[i];
        ps2::Tim2Picture p = made[i].head;
        std::string comment = s.name;
        uint32_t extended = static_cast<uint32_t>(sizeof(ps2::Tim2Extended) + sizeof(ps2::Tim2Terms) + comment.size() + 1);
        uint32_t head_bytes = ps2::extended_at(p.mipmaps) + extended;
        auto aligned = [](size_t n) {
            return static_cast<uint32_t>((n + ps2::TIM2_ALIGN - 1) & ~static_cast<size_t>(ps2::TIM2_ALIGN - 1));
        };
        ps2::Tim2Mips mips{};

        head_bytes = (head_bytes + ps2::TIM2_ALIGN - 1) & ~(ps2::TIM2_ALIGN - 1);
        p.head_bytes = static_cast<uint16_t>(head_bytes);
        p.image_bytes = aligned(made[i].texels.size());
        mips.sizes[0] = p.image_bytes;
        for (size_t l = 0; l < made[i].levels.size(); l++) {
            uint64_t tbw = made[i].level_tbw[l];

            mips.sizes[l + 1] = aligned(made[i].levels[l].size());
            p.image_bytes += mips.sizes[l + 1];
            if (l < 3) {
                mips.miptbp1 |= tbw << (14 + 20 * l);
            } else {
                mips.miptbp2 |= tbw << (14 + 20 * (l - 3));
            }
        }
        p.clut_bytes = static_cast<uint32_t>((made[i].clut.size() + ps2::TIM2_ALIGN - 1) & ~(ps2::TIM2_ALIGN - 1));
        p.total = p.head_bytes + p.image_bytes + p.clut_bytes;
        ps2::Tim2Extended ext = {{'e', 'X', 't', 0}, extended, sizeof(ps2::Tim2Terms), 0};
        ps2::Tim2Terms terms = {ps2::TERMS_MAGIC, s.global_index, made[i].flags, s.pixel, s.layout, 0,
                                static_cast<uint16_t>(s.image.width), static_cast<uint16_t>(s.image.height), 0};

        put(out, &p, sizeof p);
        if (p.mipmaps > 1) {
            put(out, &mips, sizeof mips);
        }
        put(out, &ext, sizeof ext);
        put(out, &terms, sizeof terms);
        put(out, comment.c_str(), comment.size() + 1);
        pad(out, ps2::TIM2_ALIGN);
        put(out, made[i].texels.data(), made[i].texels.size());
        pad(out, ps2::TIM2_ALIGN);
        for (const std::vector<uint8_t> &level : made[i].levels) {
            put(out, level.data(), level.size());
            pad(out, ps2::TIM2_ALIGN);
        }
        put(out, made[i].clut.data(), made[i].clut.size());
        pad(out, ps2::TIM2_ALIGN);
    }
    return out;
}

/* path written with bytes, unless it holds them already */
bool write_changed(const std::string &path, const std::vector<uint8_t> &bytes, bool &changed)
{
    std::vector<uint8_t> was;

    changed = !read_all(path, was) || was != bytes;
    if (!changed) {
        return true;
    }
    FILE *out = fopen(path.c_str(), "wb");

    return out != nullptr && fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size() && fclose(out) == 0;
}

/* a file's name, its folder and suffix gone, in capitals */
std::string name_of(const std::string &path)
{
    std::string name = path.substr(path.find_last_of('/') + 1);

    name = name.substr(0, name.find_last_of('.'));
    for (char &c : name) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    return name;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "textures_tm2 DISC DIR\n");
        return 2;
    }
    std::string root = argv[1], dir = std::string(argv[2]) + "/textures";
    pc::Disc disc;
    pc::Textures textures;
    std::vector<uint8_t> index, bytes, plain;

    mkdir(argv[2], 0755);
    mkdir(dir.c_str(), 0755);
    /* the index goes to textures.bin in the working directory, as a run's
     * does beside its program */
    if (chdir(dir.c_str()) != 0) {
        fprintf(stderr, "textures_tm2: can't go to %s\n", dir.c_str());
        return 1;
    }
    remove("textures.bin");
    disc.add_root(root);
    textures.index(disc, "SONICADV");
    if (!read_all("textures.bin", index) || index.size() < sizeof(ps2::IndexHead)) {
        fprintf(stderr, "textures_tm2: no index was written\n");
        return 1;
    }
    remove("textures.bin");
    ps2::IndexHead head;

    memcpy(&head, index.data(), sizeof head);
    const auto *files = reinterpret_cast<const ps2::IndexFile *>(index.data() + sizeof head);
    const auto *places = reinterpret_cast<const ps2::IndexPlace *>(files + head.files);
    /* each name's textures, its PVMs' first */
    std::map<std::string, std::vector<Source>> names;
    uint32_t refused = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t f = 0; f < head.files; f++) {
            const ps2::IndexFile &file = files[f];
            std::string path(file.path, strnlen(file.path, sizeof file.path));
            bool loose = path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".PVR") == 0;
            const std::vector<uint8_t> *holds = &bytes;

            if (loose != (pass == 1)) {
                continue;
            }
            if (!disc.read(path, bytes) || (file.compressed && !pc::prs_decode(bytes, plain))) {
                fprintf(stderr, "textures_tm2: can't read %s\n", path.c_str());
                refused += file.count;
                continue;
            }
            holds = file.compressed ? &plain : &bytes;
            std::vector<Source> &into = names[name_of(path)];

            for (uint32_t p = file.first; p < file.first + file.count && p < head.places; p++) {
                Source s;
                std::string why;
                const uint8_t *chunk = holds->data() + places[p].at;

                s.path = path;
                s.global_index = places[p].global_index;
                s.name.assign(places[p].name, strnlen(places[p].name, sizeof places[p].name));
                s.pixel = chunk[8];
                s.layout = chunk[9];
                if (!pc::pvr_decode(*holds, places[p].at, s.image, why) || s.image.width > ps2::SIDE_MOST ||
                    s.image.height > ps2::SIDE_MOST) {
                    fprintf(stderr, "textures_tm2: %s's %s: %s\n", path.c_str(), s.name.c_str(), why.c_str());
                    refused++;
                    continue;
                }
                /* a texture another of the name's files holds alike goes once */
                bool again = false;

                for (const Source &o : into) {
                    again |= o.global_index == s.global_index && o.image.width == s.image.width &&
                             o.image.height == s.image.height && o.image.rgba == s.image.rgba;
                }
                if (!again) {
                    into.push_back(std::move(s));
                }
            }
        }
    }
    FILE *report = fopen("report.csv.new", "w");
    std::map<std::string, uint32_t> kinds;
    uint32_t pictures = 0, written = 0, wrong = 0, mipped = 0;
    uint64_t total = 0;
    double worst = INFINITY;

    if (report == nullptr) {
        return 1;
    }
    fprintf(report, "file,picture,global_index,name,source,pixel,layout,width,height,kind,colors,psnr,most,bytes,levels\n");
    for (const auto &[name, sources] : names) {
        std::vector<Made> made;

        for (size_t i = 0; i < sources.size(); i++) {
            const Source &s = sources[i];
            Made m = make(s);
            bool whole = (m.flags & ps2::TERMS_QUANTIZED) == 0;
            /* a whole picture differs by the alpha's halving, or the
             * 16-bit's dropped bits */
            int allowed = strcmp(m.kind, "16-bit") == 0 ? 7 : 1;

            if (!whole && m.psnr < FIT_PSNR) {
                /* the fit is too far: 16 or 32 bits */
                Source plain_source = s;

                /* still mipmapped where it was, but not VQ */
                plain_source.layout = has_mips(s.layout) ? 0x02 : 0;
                m = make(plain_source);
                allowed = strcmp(m.kind, "16-bit") == 0 ? 7 : 1;
                whole = true;
            }
            if (whole && m.most > allowed) {
                fprintf(stderr, "textures_tm2: %s's %s differs by %d as %s\n", name.c_str(), s.name.c_str(),
                        m.most, m.kind);
                wrong++;
            }
            if (!whole) {
                worst = std::min(worst, m.psnr);
            }
            kinds[m.kind]++;
            pictures++;
            size_t bytes = m.texels.size() + m.clut.size();

            for (const std::vector<uint8_t> &level : m.levels) {
                bytes += level.size();
            }
            total += bytes;
            mipped += m.levels.empty() ? 0 : 1;
            fprintf(report, "%s,%zu,%u,%s,%s,%u,%u,%u,%u,%s,%u,%.2f,%d,%zu,%u\n", name.c_str(), i, s.global_index,
                    s.name.c_str(), s.path.c_str(), s.pixel, s.layout, s.image.width, s.image.height, m.kind,
                    m.colors, std::isinf(m.psnr) ? 999.0 : m.psnr, m.most, bytes,
                    static_cast<unsigned>(m.head.mipmaps));
            made.push_back(std::move(m));
        }
        bool changed = false;

        if (!write_changed(name + ".tm2", tim2_of(sources, made), changed)) {
            fprintf(stderr, "textures_tm2: can't write %s.tm2\n", name.c_str());
            return 1;
        }
        written += changed;
    }
    fclose(report);
    rename("report.csv.new", "report.csv");
    /* each texture file's name id, as the game's records keep it
     * (native_disc_file_id), and its TIM2's name, a line each */
    {
        std::string table;
        char line[96];
        bool changed = false;

        for (uint32_t f = 0; f < head.files; f++) {
            std::string path(files[f].path, strnlen(files[f].path, sizeof files[f].path));

            snprintf(line, sizeof line, "%08x %s\n", files[f].id, name_of(path).c_str());
            table += line;
        }
        if (!write_changed("files.txt", std::vector<uint8_t>(table.begin(), table.end()), changed)) {
            return 1;
        }
    }
    printf("textures_tm2: %zu files, %u pictures, %u refused, %u rewritten, %.1f MB of texels and CLUTs;",
           names.size(), pictures, refused, written, total / 1048576.0);
    for (const auto &[kind, n] : kinds) {
        printf(" %s %u,", kind.c_str(), n);
    }
    printf(" %u mipmapped, the farthest fit %.1f dB\n", mipped, std::isinf(worst) ? 999.0 : worst);
    if (wrong != 0) {
        fprintf(stderr, "textures_tm2: %u pictures differ from their images past their format's loss\n", wrong);
        return 1;
    }
    return 0;
}
