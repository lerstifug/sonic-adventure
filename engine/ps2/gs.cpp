/* the PS2's raster: SA1's adapter's draws (engine/psp/draw.h) and the scene's
 * batches (engine/psp/direct.cpp), drawn by the GS through gsKit. the PSP's
 * raster (engine/psp/raster.cpp) gives the terms this one keeps: a picture
 * is the game's 640 by 480, a view's point shows at center + focal * (x, y)
 * / z, the near plane lies at z = 1, and the depth buffer holds 1/z, the
 * nearer the greater, as the PowerVR's does.
 *
 * the GS transforms nothing and clips nothing but to its scissor, so the EE
 * does both: each triangle goes to view space, is cut to the near plane and
 * to REACH half-frames to each side, within the 4096 pixels the GS's
 * coordinates span, and goes to the GS as RGBAQ, STQ and XYZF2, which give a
 * texture its perspective and a corner its fog. the vector units can take
 * this work later.
 *
 * what the PowerVR does that the PSP's raster leaves out, this one draws:
 *
 * - the offset color, the highlight a lit point adds to its texel. an
 *   opaque or cut texture takes it by the GS's HIGHLIGHT2, the texel times
 *   the corner's color plus the corner's alpha, which holds the offset's
 *   brightest of three. a blend takes it by a second pass that adds the
 *   offset over what the first drew, at the same depth.
 * - fog. each picture reads the PowerVR's fog color, density and table from
 *   its registers, as Ninja wrote them, and a corner takes the table's fog
 *   for its 1/z, as the PowerVR's pixel does (engine/pc/shaders/draw.frag),
 *   or the alpha of its offset under fog by its points.
 * - the blend list's order. the PowerVR sorts what blends by the pixel.
 *   here each triangle of the picture that blends, the walk's and the
 *   scene's alike, waits for the picture's opaque draws, and they all go
 *   the farthest first, by each triangle's middle. what the scene draws
 *   on the screen's own pixels goes after, in the game's order.
 * - a texture mirrored past 1 in u or v, by a copy of it mirrored, made at
 *   its first such use.
 *
 * the screen is 640 by 480 at 16 bits, with a 16-bit depth buffer: two
 * screens and the depth take 1.8 MB of the GS's 4 MB, and its texture
 * manager keeps the textures the picture uses in the rest, sent from the
 * EE's memory as a picture binds them. a texture lies in the EE's memory at
 * power-of-two sides, at 32 bits where its alpha has steps between clear
 * and solid, else at 16. the GS's 1.0 is 0x80, so a modulating corner's
 * color and every alpha go in halved */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

extern "C" {
#include <dmaKit.h>
#include <gsKit.h>
#include <gsInline.h>
#include <kernel.h>

#include "psp.h"
}

#include "draw.h"
#include "pspkernel.h"
#include "pvr.h"
#include "raster.h"
#include "texels.h"
#include "tim2.h"

/* VU1's program for a strip (engine/ps2/strip.vsm) */
extern "C" uint32_t VU1Strip_CodeStart __attribute__((section(".vudata")));
extern "C" uint32_t VU1Strip_CodeEnd __attribute__((section(".vudata")));
/* and for a model's batch (engine/ps2/model.vsm) */
extern "C" uint32_t VU1Model_CodeStart __attribute__((section(".vudata")));
extern "C" uint32_t VU1Model_CodeEnd __attribute__((section(".vudata")));

/* the walk's figures (engine/psp/sa1/walk.h) */
extern "C" uint32_t ps2_walk_load_us, ps2_walk_light_us;

/* the screen's mode, as the settings give it (engine/ps2/main.c): 0 for
 * 480p, the default, 1 for 480i, and in 480i whether the merge circuit
 * filters the flicker, each line shown with half of the line below */
extern "C" int ps2_interlaced, ps2_deflicker;
/* whether the GS dithers the 16-bit screen (engine/ps2/main.c's dither) */
extern "C" int ps2_dither;
/* whether the run times the raster's strips and binds one by one, as the
 * settings' profile 1 asks: the clock takes its own time at each */
extern "C" int ps2_profile;
/* whether opaque strips go to VU1 (engine/ps2/main.c's vu) */
extern "C" int ps2_vu;

static inline uint32_t stamp()
{
    return ps2_profile ? sceKernelGetSystemTimeLow() : 0;
}

namespace pc {

/* direct.cpp's figure of the pictures' waits for the display */
uint32_t psp_wait_us;
/* textures.cpp's: a lender's texture is gone, and no stream reads its
 * texels */
void ps2_texture_returned(void *lender);

namespace {

/* the game's own 640 by 480, shown progressive at 480p, or interlaced at
 * 480i, with a filter of its flicker to turn on or off */
constexpr int SCREEN_W = 640;
/* the picture's height on the screen: the game's 480 lines at 480p, and
 * at 480i gsKit's 448, the picture cropped by CROP_480I lines at its top and
 * as many at its bottom, so it stays centered */
constexpr int SCREEN_H = 480;
constexpr int SCREEN_H_480I = 448;
constexpr float CROP_480I = (SCREEN_H - SCREEN_H_480I) / 2;

int screen_height()
{
    return ps2_interlaced ? SCREEN_H_480I : SCREEN_H;
}

/* the lines the picture moves up on the screen */
float crop()
{
    return ps2_interlaced ? CROP_480I : 0.0F;
}
constexpr float NEAR_Z = 1.0F;
/* how far past the frame a corner may go, in half-frames: 5 keeps every
 * corner within the GS's coordinates, from -2048 to 2047 past the frame's
 * middle */
constexpr float REACH = 5.0F;
constexpr float DEPTH_MOST = 65535.0F;
/* the qwords of each of VIF1's two streams, a picture's commands: a
 * picture that fills one sends it and goes on in the other */
constexpr uint32_t STREAM_QWORDS = 3U << 15;
/* the words a stream keeps spare past any write, for its last tags */
constexpr uint32_t STREAM_SPARE = 64;
/* the most qwords of a DMA tag's, of a DIRECT's and of a GIF tag's loops */
constexpr uint32_t TAG_MOST = 65535, DIRECT_MOST = 65535, IMAGE_MOST = 32767;
/* VIF1's codes */
constexpr uint32_t VIF_STCYCL = 0x01, VIF_OFFSET = 0x02, VIF_BASE = 0x03, VIF_FLUSHE = 0x10, VIF_FLUSH = 0x11,
                   VIF_MSCAL = 0x14, VIF_DIRECT = 0x50, VIF_MPG = 0x4a;
constexpr uint32_t UNPACK_V4_32 = 0x6c, UNPACK_V3_32 = 0x68, UNPACK_V2_16 = 0x65, UNPACK_V4_8 = 0x6e;
constexpr uint32_t UNPACK_USN = 0x4000, UNPACK_TOPS = 0x8000;
/* DMA tags' ids */
constexpr uint32_t TAG_CNT = 1, TAG_REF = 3, TAG_END = 7;
/* the GS's memory in blocks of 256 bytes, the unit a texture's place is in */
constexpr uint32_t VRAM_BLOCKS = 4U * 1024 * 1024 / 256;
/* VU1's memory: the fog table's 64 qwords, the palette's 256 a model's
 * batch lights by (model.vsm), then two batches' 352 each */
constexpr uint32_t VU_FOG = 64, VU_PALETTE = 64, VU_PALETTE_ENTRIES = 256, VU_BASE = 320, VU_OFFSET = 352;
/* the corners a batch of VU1's takes at most: its 12 qwords of head, 4 of
 * each corner and 3 of each corner sent fit in 352 */
constexpr uint32_t VU_CORNERS = 48;
static_assert(12 + VU_CORNERS * 7 + 1 <= VU_OFFSET, "a strip batch passes VU1's buffer");
/* where VU1's model program lies in its code memory, in instructions, past
 * the strip program's; and a model batch's head, in qwords: its corners,
 * two qwords each, and the two qwords each sends, fit in 352 with it */
constexpr uint32_t VU_MODEL_AT = 512, VU_MODEL_HEAD = 15;
static_assert(VU_MODEL_HEAD + PS2_VU_CORNERS * 4 + 1 <= VU_OFFSET, "a model batch passes VU1's buffer");
static_assert(PS2_VU_CORNERS * 2 <= 256, "a model batch's corners pass an UNPACK's count");
/* the corners of the strips kept for the picture's end */
constexpr uint32_t KEPT_CORNERS = 1U << 14;
/* the triangles that blend a picture holds for its sort. past them a
 * triangle draws as it comes */
constexpr uint32_t BLENDS_MOST = 1U << 12;
/* a texture's longest side the EE keeps: 1024 texels, the GS's most */
constexpr uint32_t SIDE_MOST = 1024;
/* the longest side a texture mirrored past 1 has a mirrored copy made of */
constexpr uint32_t MIRROR_MOST = 128;
/* the PowerVR's registers a picture's fog reads, by their places among the
 * PVR's (engine/sa1/records.c) */
constexpr uint32_t FOG_COLOR = 0xb0;
constexpr uint32_t FOG_DENSITY = 0xb8;
constexpr uint32_t FOG_TABLE = 0x200;
/* the GS's texture functions */
constexpr int TFX_MODULATE = 0, TFX_DECAL = 1, TFX_HIGHLIGHT2 = 3;
/* the GS's XYZF2, a corner's place with its fog */
constexpr uint64_t GS_XYZF2_REG = 0x04;
constexpr uint64_t GS_FOGCOL_REG = 0x3d;

struct Texture {
    GSTEXTURE gs{};
    uint32_t width = 0, height = 0; /* the image's own */
    float su = 1.0F, sv = 1.0F; /* the image's share of the stored sides */
    uint8_t tw = 0, th = 0; /* the stored sides' log2 */
    bool cuts = false;
    /* the copies mirrored in u, in v, and in both, made at their first use */
    Texture *mirrored[3] = {nullptr, nullptr, nullptr};
    /* its place in the GS's memory, in blocks, -1 where it lies there no
     * more, the blocks it takes, its texels' alone, and the picture that
     * last bound it */
    int32_t block = -1;
    uint32_t blocks = 0, texel_blocks = 0;
    uint32_t used = 0;
    /* a 4-bit or 8-bit texture's CLUT: 16 or 256 colors at 32 bits, as the
     * GS's CSM1 takes them */
    const void *clut = nullptr;
    uint32_t clut_colors = 0;
    /* where its texels and CLUT lie in another's memory, the lender's handle
     * the texture's drop gives back (textures.cpp), else null: it owns its
     * texels */
    void *lender = nullptr;
    /* a place of its own in the GS's memory, above the VRAM manager's,
     * taken at its first bind and never given up: the texture stays past
     * the game's release */
    bool fixed = false;
};

/* what a draw is drawn under: a strip's psp_state, or a batch's State */
struct Draw {
    const Texture *texture = nullptr;
    bool decal = false, alpha = true; /* the texture's alpha counts */
    bool clamp_u = false, clamp_v = false, linear = true, cut = false;
    bool blend = false;
    uint8_t src = SA_BLEND_ONE, dst = SA_BLEND_ZERO;
    uint8_t depth = SA_DEPTH_GEQUAL;
    bool depth_write = true;
    uint8_t cull = 0; /* 1 drops what turns clockwise, 2 what turns the other way */
    bool offset = false; /* the offset adds to the texel */
    uint8_t fog = SA_FOG_NONE;

    bool same(const Draw &o) const
    {
        return texture == o.texture && decal == o.decal && alpha == o.alpha && clamp_u == o.clamp_u &&
               clamp_v == o.clamp_v && linear == o.linear && cut == o.cut && blend == o.blend && src == o.src &&
               dst == o.dst && depth == o.depth && depth_write == o.depth_write && offset == o.offset &&
               fog == o.fog;
    }
};

/* a corner in view space, with its texture's place over the stored sides,
 * its color in the GS's terms, its offset's three, and the fog its points
 * give it, of 1 */
struct Corner {
    float x, y, z;
    float u, v;
    float r, g, b, a;
    float or_, og, ob, fog;
};

/* a corner made for the GS: its corner, its pixel, depth, 1/z and fog */
struct Sent {
    Corner v;
    float x, y, z, q;
    uint8_t f;
};

/* a triangle that waits for the picture's sort: its draw, its key of depth,
 * and its corners, the three from first in the picture's blend corners */
struct Blend {
    Draw d;
    uint32_t key;
    uint32_t first;
};

struct Placed {
    float matrix[16];
    float screen[4];
};

struct Later {
    float depth;
    uint32_t place;
    psp_state state;
    uint32_t first, count;
    bool triangles; /* else a strip */
};

/* the picture's fog, as the PowerVR's registers hold it */
struct Fog {
    float density = 0.0F;
    uint8_t table[128][2]{}; /* each entry's high and low */
    uint32_t color = 0;
    /* the GS's fog for 1/z times the density, held to 1 to 256: by the
     * value's exponent, 0 to 7, then its mantissa's top 7 bits, each the
     * table's fog at the step's middle, made as the picture opens */
    uint8_t gs[8 << 7]{};
};

struct Held {
    GSGLOBAL *gs = nullptr;
    bool opened = false;
    bool picture = false; /* a picture's commands lie open */
    std::vector<Texture *> textures; /* by index, less 1 */
    /* the walk's place and screen of the moment, and its state */
    Placed place{};
    bool place_kept = false;
    psp_state state{};
    std::vector<Placed> places;
    std::vector<Later> later;
    std::vector<psp_corner> room; /* a strip's corners, drawn at once */
    psp_corner *kept = nullptr; /* the kept strips' */
    uint32_t kept_used = 0;
    /* the triangles that blend, waiting for the picture's sort */
    std::vector<Blend> blends;
    Fog fog;
    /* the picture's own color, which the next picture opens on: the game
     * gives it at the picture's end, after its draws */
    uint32_t clear = 0xff000000U;
    /* the last registers sent, so a draw sends only what it changes */
    uint64_t tex0 = 0, tex1 = 0, clamp = 0, alpha = 0, test = 0, zbuf = 0;
    bool sent = false;
    std::vector<Corner> cut_in, cut_out;
    std::vector<Sent> sent_corners;
    std::vector<uint8_t> made_texels; /* an image's texels, as made */
    /* a cut strip's triangles, kept from call to call: each new vector a
     * call made would take the C library's lock, a system call each */
    std::vector<psp_corner> cut_strip;
    /* pictures opened over one still open, whose commands went unsent */
    uint32_t taken_back = 0;
    /* a draw's corners' sides, the indices of its triangles kept, a list's
     * own order, and a strip's triangles */
    std::vector<uint32_t> masks, kept_index, order, strip_index;
    /* the waiting blends' corners, three to a blend, and their order */
    std::vector<Sent> blend_corners;
    std::vector<uint32_t> blend_order;
    /* the texture last bound this picture: another bind of it sends
     * nothing, so it binds once until another binds */
    const Texture *bound = nullptr;
    /* the screen buffer the open picture draws into, 0 or 1: the other
     * shows, or shows from the next blank (flip_at_blank) */
    uint32_t draw_buffer = 0;
    /* VIF1's two streams, as DMA chains: the one the EE writes, its words
     * written, its open CNT tag's word and its open DIRECT's, -1 where
     * none. a DIRECT after VU1's work waits for it by a FLUSH */
    uint32_t *stream[2] = {nullptr, nullptr};
    uint32_t writing = 0, at = 0;
    int32_t tag = -1, direct = -1;
    bool vu_since = false;
    /* whether the picture sent VU1 its fog table */
    bool vu_fog = false;
    /* the open model batch's head's first word and its corners' UNPACK's,
     * and its GIF tag's PRIM: ps2_vu_open */
    uint32_t model_head = 0, model_unpack = 0;
    uint64_t model_prim = 0;
    bool model_open = false;
    /* the palette in VU1's memory, by ps2_vu_palette's key, none at a
     * picture's open; and the picture's sends of one */
    uint32_t vu_palette = UINT32_MAX;
    uint32_t palettes_sent = 0;
    /* the textures whose texels the unsent stream sends: a picture taken
     * back takes them back, and they lie in the GS's memory no more */
    std::vector<Texture *> uploads;
    /* texels freed while a stream may send them, freed once it's sent */
    std::vector<void *> dying, dying_sent;
    /* the textures in the GS's memory, by place, and its first block past
     * the screens and the depth */
    std::vector<Texture *> resident;
    uint32_t vram_first = 0;
    /* the fixed textures' room, from fixed_low to the top, and its blocks
     * in use */
    uint32_t fixed_low = VRAM_BLOCKS, fixed_blocks = 0;
    /* lenders whose texels a stream unsent or sending may still read: their
     * return waits for it, as texels' freeing does */
    std::vector<void *> returns, returns_sent;
    /* figures for psp_draw_report */
    uint32_t pictures = 0, lost = 0, most_textures = 0, texture_bytes = 0;
    uint32_t most_queue = 0, most_blends = 0, blends_past = 0, mirrors = 0;
    /* since the last report: pictures, triangles sent, and the
     * microseconds the EE took making them and waited on the GS */
    uint32_t timed = 0, triangles = 0;
    uint64_t make_us = 0, gs_us = 0, strip_us = 0, bind_us = 0;
    uint32_t sends = 0; /* the textures a bind sent */
    uint32_t vu_batches = 0, vu_triangles = 0; /* VU1's batches and their triangles */
    uint32_t strips_past = 0; /* strips wholly past a side, dropped */
    uint32_t reported = 0; /* the clock at the last report */
    /* the figures as the picture opened, for a slow one's own */
    uint32_t picture_triangles = 0, picture_sends = 0, picture_load_us = 0;
    uint64_t picture_strip_us = 0, picture_bind_us = 0;
    uint32_t made_since = 0; /* the clock when the picture opened */
};
Held held;

uint32_t bits(float f)
{
    uint32_t b;

    memcpy(&b, &f, sizeof b);
    return b;
}

/* a depth's key for the blends' sort. a positive float's bits order as its
 * value does, so its top bits past 1.0's are keys in its order, fine near
 * the eye and coarser far from it: 2^-9 of a depth's own size apart. a
 * depth past the last key's takes the last */
constexpr uint32_t DEPTH_KEYS = 1U << 13;

uint32_t depth_key(float depth)
{
    uint32_t b = bits(depth < 1.0F ? 1.0F : depth);
    uint32_t key = (b - 0x3f800000U) >> 14;

    return key < DEPTH_KEYS ? key : DEPTH_KEYS - 1;
}

uint32_t side_log2(uint32_t n)
{
    uint32_t l = 0;

    while ((1U << l) < n) {
        l++;
    }
    return l;
}

/* the commands go to the GS by one stream of VIF1's, in their order: the
 * EE's own packets of the GIF's by DIRECT, VU1's batches, which VU1 sends
 * the GIF by XGKICK, and the texels of a texture by DIRECT from where the
 * EE keeps them, by a REF tag of the stream's DMA chain. the GIF takes
 * VU1's path first where both wait, so a DIRECT after VU1's work waits for
 * it to end. a stream goes at the picture's end, while the EE writes the
 * other, or sooner where it fills */
volatile uint32_t *const D1_CHCR = reinterpret_cast<volatile uint32_t *>(0x10009000);
volatile uint32_t *const D1_MADR = reinterpret_cast<volatile uint32_t *>(0x10009010);
volatile uint32_t *const D1_QWC = reinterpret_cast<volatile uint32_t *>(0x10009020);
volatile uint32_t *const D1_TADR = reinterpret_cast<volatile uint32_t *>(0x10009030);
constexpr uint32_t CHCR_STR = 0x100;

/* the buffer the display shows from the next blank, or -1: flip_at_blank */
volatile int32_t flip_pending = -1;

uint32_t vif_code(uint32_t command, uint32_t num, uint32_t immediate)
{
    return immediate | num << 16 | command << 24;
}

uint32_t *words(Held &r)
{
    return r.stream[r.writing];
}

void put(Held &r, uint32_t word)
{
    words(r)[r.at++] = word;
}

uint32_t physical(const void *p)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) & 0x0fffffffU;
}

void vif_wait()
{
    while ((*D1_CHCR & CHCR_STR) != 0) {
    }
}

/* the open DIRECT's count, its qwords since */
void close_direct(Held &r)
{
    if (r.direct >= 0) {
        words(r)[r.direct] = vif_code(VIF_DIRECT, 0, (r.at - static_cast<uint32_t>(r.direct) - 1) / 4);
        r.direct = -1;
    }
}

/* a CNT tag opens where none is open, at a qword's start */
void open_tag(Held &r)
{
    if (r.tag < 0) {
        r.tag = static_cast<int32_t>(r.at);
        r.at += 4;
    }
}

/* the open CNT tag takes its qwords since: the words to a qword's end are
 * VIF1's NOPs */
void close_tag(Held &r)
{
    close_direct(r);
    if (r.tag < 0) {
        return;
    }
    while (r.at % 4 != 0) {
        put(r, 0);
    }
    uint32_t *t = words(r) + r.tag;
    uint32_t qwords = (r.at - static_cast<uint32_t>(r.tag)) / 4 - 1;

    if (qwords == 0) {
        r.at = static_cast<uint32_t>(r.tag);
    } else {
        t[0] = qwords | TAG_CNT << 28;
        t[1] = t[2] = t[3] = 0;
    }
    r.tag = -1;
}

void send_stream(Held &r, bool last);

/* room for count words in the open CNT tag: a stream that would fill goes
 * at once, and a tag that would pass its most closes */
void room(Held &r, uint32_t count)
{
    if (r.at + count + STREAM_SPARE > STREAM_QWORDS * 4) {
        send_stream(r, false);
    }
    open_tag(r);
    if ((r.at - static_cast<uint32_t>(r.tag) + count) / 4 + 2 > TAG_MOST) {
        close_tag(r);
        open_tag(r);
    }
}

/* one of VIF1's own codes */
void vif(Held &r, uint32_t code)
{
    room(r, 4);
    close_direct(r);
    put(r, code);
}

/* room for qwords of the GIF's in the open DIRECT, or a new one's */
u64 *gif(Held &r, uint32_t qwords)
{
    room(r, qwords * 4 + 8);
    if (r.direct >= 0 && (r.at - static_cast<uint32_t>(r.direct) - 1) / 4 + qwords > DIRECT_MOST) {
        close_direct(r);
    }
    if (r.direct < 0) {
        if (r.vu_since) {
            put(r, vif_code(VIF_FLUSH, 0, 0));
            r.vu_since = false;
        }
        /* a DIRECT's qwords start at a qword's start */
        while (r.at % 4 != 3) {
            put(r, 0);
        }
        r.direct = static_cast<int32_t>(r.at);
        put(r, 0);
    }
    u64 *p = reinterpret_cast<u64 *>(words(r) + r.at);

    r.at += qwords * 4;
    return p;
}

/* the GS's texels from the EE's memory, qwords of them, after their
 * transfer's registers: each part of a GIF tag's most by its own DIRECT,
 * its texels by a REF tag. texels the stream sends lie as they are until
 * it's sent */
void send_texels(Held &r, const void *texels, uint32_t qwords)
{
    const uint8_t *from = static_cast<const uint8_t *>(texels);

    for (uint32_t done = 0; done < qwords;) {
        uint32_t n = std::min(qwords - done, IMAGE_MOST);

        room(r, 16);
        close_direct(r);
        if (r.vu_since) {
            put(r, vif_code(VIF_FLUSH, 0, 0));
            r.vu_since = false;
        }
        while (r.at % 4 != 3) {
            put(r, 0);
        }
        put(r, vif_code(VIF_DIRECT, 0, n + 1));
        u64 *p = reinterpret_cast<u64 *>(words(r) + r.at);

        p[0] = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        p[1] = 0;
        r.at += 4;
        close_tag(r);
        uint32_t *t = words(r) + r.at;

        t[0] = n | TAG_REF << 28;
        t[1] = physical(from + done * 16);
        t[2] = t[3] = 0;
        r.at += 4;
        done += n;
    }
}

/* the stream so far to VIF1, once the one before is sent, and the EE
 * writes the other. a stream draws into the picture's buffer, so it waits
 * for the last picture's flip, the buffer showing till then. the last of
 * a picture's ends in the GS's FINISH, which the flip waits for */
void send_stream(Held &r, bool last)
{
    close_tag(r);
    uint32_t *t = words(r) + r.at;

    t[0] = TAG_END << 28;
    t[1] = t[2] = t[3] = 0;
    r.at += 4;
    r.most_queue = std::max(r.most_queue, r.at * 4);
    while (flip_pending >= 0) {
    }
    vif_wait();
    for (void *p : r.dying_sent) {
        free(p);
    }
    r.dying_sent.swap(r.dying);
    r.dying.clear();
    for (void *lender : r.returns_sent) {
        ps2_texture_returned(lender);
    }
    r.returns_sent.swap(r.returns);
    r.returns.clear();
    r.uploads.clear();
    if (last) {
        *GS_CSR = 2;
    }
    FlushCache(0);
    *D1_QWC = 0;
    *D1_TADR = physical(words(r));
    *D1_CHCR = 0x105; /* from memory, a chain, no tags to VIF1 */
    r.writing ^= 1;
    r.at = 0;
    r.tag = -1;
    r.direct = -1;
}

/* registers by address and data, as an A+D packet. count is at most 8 */
void send_registers(Held &r, const uint64_t *values, const uint64_t *names, int count)
{
    if (count == 0) {
        return;
    }
    u64 *p = gif(r, static_cast<uint32_t>(count) + 1);

    *p++ = GIF_TAG_AD(count);
    *p++ = GIF_AD;
    for (int i = 0; i < count; i++) {
        *p++ = values[i];
        *p++ = names[i];
    }
}

/* the primitive the next REGLIST packet draws: its GIF tag's own PRIM is
 * read in PACKED mode alone, so the register goes ahead of it */
void set_prim(Held &r, uint64_t prim)
{
    uint64_t name = GS_PRIM;

    send_registers(r, &prim, &name, 1);
}

/* the GS's depth test for State's compares, on 1/z. the GS has no test
 * that passes the farther, so those pass always */
uint64_t depth_test(uint8_t depth)
{
    switch (depth & 7) {
    case SA_DEPTH_NEVER:
        return 0;
    case SA_DEPTH_GREATER:
        return 3;
    case SA_DEPTH_GEQUAL:
    case SA_DEPTH_EQUAL:
        return 2;
    default:
        return 1;
    }
}

/* the GS's blend for the PowerVR's two factors: (A - B) * C + D, of the
 * source's color, the screen's and zero, by the source's alpha or a fixed
 * one. a pair it can't give takes the usual alpha blend */
uint64_t blend_of(uint8_t src, uint8_t dst)
{
    enum { CS = 0, CD = 1, ZERO = 2, AS = 0, FIX = 2 };

    if (src == SA_BLEND_SRC_ALPHA && dst == SA_BLEND_ONE) {
        return GS_SETREG_ALPHA(CS, ZERO, AS, CD, 0);
    }
    if (src == SA_BLEND_ONE && dst == SA_BLEND_ONE) {
        return GS_SETREG_ALPHA(CS, ZERO, FIX, CD, 0x80);
    }
    if (src == SA_BLEND_ONE && dst == SA_BLEND_ZERO) {
        return GS_SETREG_ALPHA(CS, ZERO, FIX, ZERO, 0x80);
    }
    if (src == SA_BLEND_SRC_ALPHA && dst == SA_BLEND_ZERO) {
        return GS_SETREG_ALPHA(CS, ZERO, AS, ZERO, 0);
    }
    if (src == SA_BLEND_ZERO && dst == SA_BLEND_INV_SRC_ALPHA) {
        return GS_SETREG_ALPHA(ZERO, CD, AS, CD, 0);
    }
    if (src == SA_BLEND_ZERO && dst == SA_BLEND_ONE) {
        return GS_SETREG_ALPHA(CS, CS, FIX, CD, 0);
    }
    if (src == SA_BLEND_INV_SRC_ALPHA && dst == SA_BLEND_SRC_ALPHA) {
        return GS_SETREG_ALPHA(CD, CS, AS, CS, 0);
    }
    if (src == SA_BLEND_ONE && dst == SA_BLEND_INV_SRC_ALPHA) {
        /* Cs + Cd * (1 - As): the GS adds Cs whole where (0 - Cd) * As + Cd
         * would need a fourth term, so this takes the alpha blend's */
        return GS_SETREG_ALPHA(CS, CD, AS, CD, 0);
    }
    return GS_SETREG_ALPHA(CS, CD, AS, CD, 0);
}

/* whether a draw takes its offset in the same pass, by HIGHLIGHT2 */
bool highlights(const Draw &d)
{
    return d.offset && d.texture != nullptr && !d.decal && !d.blend;
}

/* the GS's 4-bit and 8-bit formats, whose texels index a CLUT */
constexpr uint32_t PSM_T8 = 0x13, PSM_T4 = 0x14;

/* a format's bits a texel */
uint32_t texel_bits(uint32_t psm)
{
    return psm == GS_PSM_CT32 ? 32 : psm == PSM_T8 ? 8 : psm == PSM_T4 ? 4 : 16;
}

/* the TBW of a texture w texels wide: 64 a unit, and for 4 and 8 bits,
 * whose pages are 128 wide, an even count */
uint32_t tbw_of(uint32_t w, uint32_t psm)
{
    uint32_t units = (w + 63) / 64;

    return psm == PSM_T8 || psm == PSM_T4 ? std::max<uint32_t>(2, (units + 1) & ~1U) : std::max<uint32_t>(1, units);
}

/* the blocks a texture of w by h takes in the GS's memory. its pages lie in
 * rows of TBW: 64 by 32 texels at 32 bits, 64 by 64 at 16, 128 by 64 at 8
 * and 128 by 128 at 4. within a page its blocks lie out of order, 32 and 8
 * bits' by one table and 16 and 4 bits' by another: a texture within one
 * page takes the blocks to the last it reaches */
uint32_t blocks_of(uint32_t w, uint32_t h, uint32_t psm)
{
    static const uint8_t blocks32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                           {2, 3, 6, 7, 18, 19, 22, 23},
                                           {8, 9, 12, 13, 24, 25, 28, 29},
                                           {10, 11, 14, 15, 26, 27, 30, 31}};
    static const uint8_t blocks16[8][4] = {{0, 2, 8, 10},    {1, 3, 9, 11},    {4, 6, 12, 14},   {5, 7, 13, 15},
                                           {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
    bool table32 = psm == GS_PSM_CT32 || psm == PSM_T8;
    uint32_t page_w = psm == PSM_T8 || psm == PSM_T4 ? 128 : 64;
    uint32_t page_h = psm == GS_PSM_CT32 ? 32 : psm == PSM_T4 ? 128 : 64;
    uint32_t block_w = page_w / (table32 ? 8 : 4), block_h = page_h / (table32 ? 4 : 8);
    uint32_t most = 0;

    if (w >= page_w || h >= page_h) {
        return (w + page_w - 1) / page_w * ((h + page_h - 1) / page_h) * 32;
    }
    for (uint32_t by = 0; by < (h + block_h - 1) / block_h; by++) {
        for (uint32_t bx = 0; bx < (w + block_w - 1) / block_w; bx++) {
            most = std::max<uint32_t>(most, (table32 ? blocks32[by][bx] : blocks16[by][bx]) + 1U);
        }
    }
    return most;
}

/* a texture's blocks: its texels', then its CLUT's where it has one, 16
 * colors as 8 by 2 and 256 as 16 by 16 at 32 bits */
void size_texture(Texture *t)
{
    t->texel_blocks = blocks_of(t->gs.Width, t->gs.Height, t->gs.PSM);
    t->blocks = t->texel_blocks +
                (t->clut_colors == 0 ? 0 : t->clut_colors == 16 ? blocks_of(8, 2, GS_PSM_CT32) : blocks_of(16, 16, GS_PSM_CT32));
}

/* the first place that holds blocks at align below the fixed textures'
 * room, or -1 */
int32_t find_room(const Held &r, uint32_t blocks, uint32_t align)
{
    uint32_t at = r.vram_first;

    for (const Texture *t : r.resident) {
        uint32_t start = (at + align - 1) & ~(align - 1);

        if (start + blocks <= static_cast<uint32_t>(t->block)) {
            return static_cast<int32_t>(start);
        }
        at = std::max(at, static_cast<uint32_t>(t->block) + t->blocks);
    }
    at = (at + align - 1) & ~(align - 1);
    return at + blocks <= r.fixed_low ? static_cast<int32_t>(at) : -1;
}

void unplace(Held &r, Texture *t)
{
    if (t->block >= 0) {
        r.resident.erase(std::find(r.resident.begin(), r.resident.end(), t));
        t->block = -1;
    }
}

/* t in the GS's memory, its texels sent where it lies there no more: in
 * the first room for it, made by putting out the textures bound longest
 * ago. one bound earlier in the picture may go: the GS draws what used it
 * before the texels over it come. false where it can't fit */
/* a picture's transfer to the GS's memory at block: its registers, then its
 * bytes from the EE's memory */
void transfer(Held &r, uint32_t block, uint32_t tbw, uint32_t psm, uint32_t w, uint32_t h, const void *bytes)
{
    uint64_t values[4] = {GS_SETREG_BITBLTBUF(0, 0, 0, block, tbw, psm), GS_SETREG_TRXPOS(0, 0, 0, 0, 0),
                          GS_SETREG_TRXREG(w, h), 0};
    uint64_t names[4] = {GS_BITBLTBUF, GS_TRXPOS, GS_TRXREG, GS_TRXDIR};

    send_registers(r, values, names, 4);
    send_texels(r, bytes, (w * h * texel_bits(psm) / 8 + 15) / 16);
}

/* a fixed texture's place: the room above the VRAM manager's, taken once,
 * downward from the top. what lies there of the manager's goes out */
int32_t place_fixed(Held &r, Texture *t)
{
    uint32_t align = t->blocks >= 32 ? 32 : 1;
    uint32_t at = (r.fixed_low - t->blocks) & ~(align - 1);

    if (r.fixed_low < t->blocks || at < r.vram_first + 1024) {
        return -1;
    }
    for (size_t i = r.resident.size(); i-- > 0;) {
        Texture *o = r.resident[i];

        if (static_cast<uint32_t>(o->block) + o->blocks > at) {
            unplace(r, o);
        }
    }
    r.fixed_low = at;
    r.fixed_blocks += t->blocks;
    return static_cast<int32_t>(at);
}

bool place(Held &r, Texture *t)
{
    t->used = r.pictures;
    if (t->block >= 0) {
        return true;
    }
    uint32_t align = t->blocks >= 32 ? 32 : 1;
    int32_t at;

    if (t->fixed) {
        at = place_fixed(r, t);
        if (at < 0) {
            t->fixed = false;
        }
    }
    if (!t->fixed) {
        while ((at = find_room(r, t->blocks, align)) < 0) {
            Texture *oldest = nullptr;

            for (Texture *o : r.resident) {
                if (o != t && (oldest == nullptr || o->used < oldest->used)) {
                    oldest = o;
                }
            }
            if (oldest == nullptr) {
                return false;
            }
            unplace(r, oldest);
        }
    }
    t->block = at;
    if (!t->fixed) {
        r.resident.insert(std::upper_bound(r.resident.begin(), r.resident.end(), t,
                                           [](const Texture *a, const Texture *b) { return a->block < b->block; }),
                          t);
    }
    /* the texels, then the CLUT after them */
    transfer(r, static_cast<uint32_t>(at), t->gs.TBW, t->gs.PSM, t->gs.Width, t->gs.Height, t->gs.Mem);
    if (t->clut_colors != 0) {
        bool sixteen = t->clut_colors == 16;

        transfer(r, static_cast<uint32_t>(at) + t->texel_blocks, 1, GS_PSM_CT32, sixteen ? 8 : 16, sixteen ? 2 : 16,
                 t->clut);
    }
    r.uploads.push_back(t);
    r.sends++;
    /* the GS reads the new texels after a TEXFLUSH, which the next TEX0
     * sends */
    r.tex0 = 0;
    return true;
}

/* send what d changes of the registers. a texture binds first, which may
 * send its texels ahead of the draw. tfx overrides the draw's texture
 * function where it isn't -1 */
void apply(Held &r, const Draw &d)
{
    uint64_t values[8], names[8];
    int count = 0;
    uint64_t tex0 = r.tex0, tex1, clamp, alpha, test, zbuf;

    if (d.texture != nullptr) {
        Texture *t = const_cast<Texture *>(d.texture);
        /* a texture moves only at a bind, so the texture last bound stays
         * where it lies until another binds */
        if (t != r.bound) {
            uint32_t began = stamp();

            if (!place(r, t)) {
                fprintf(stderr, "sa1: a %ux%u texture is past the GS's memory\n", static_cast<unsigned>(t->gs.Width),
                        static_cast<unsigned>(t->gs.Height));
            }
            r.bind_us += stamp() - began;
            r.bound = t;
        }
        unsigned int block = t->block >= 0 ? static_cast<unsigned>(t->block) : r.vram_first;
        int tfx = d.decal ? TFX_DECAL : highlights(d) ? TFX_HIGHLIGHT2 : TFX_MODULATE;
        /* a CLUT's texture loads its CLUT, at 32 bits after its texels, as
         * its TEX0 is set */
        bool clut = t->clut_colors != 0;

        tex0 = GS_SETREG_TEX0(block, t->gs.TBW, t->gs.PSM, t->tw, t->th, d.alpha ? 1 : 0, tfx,
                              clut ? block + t->texel_blocks : 0, 0, 0, 0, clut ? 1 : 0);
    }
    tex1 = GS_SETREG_TEX1(0, 0, d.linear ? 1 : 0, d.linear ? 1 : 0, 0, 0, 0);
    clamp = GS_SETREG_CLAMP(d.clamp_u ? 1 : 0, d.clamp_v ? 1 : 0, 0, 0, 0, 0);
    alpha = blend_of(d.src, d.dst);
    test = GS_SETREG_TEST(d.cut ? 1 : 0, 6, 0x40, 0, 0, 0, 1, depth_test(d.depth));
    zbuf = GS_SETREG_ZBUF(r.gs->ZBuffer / 8192, r.gs->PSMZ, d.depth_write && !d.blend ? 0 : 1);
    if (d.texture != nullptr && (!r.sent || tex0 != r.tex0)) {
        values[count] = 0;
        names[count++] = GS_TEXFLUSH;
        values[count] = tex0;
        names[count++] = GS_TEX0_1;
    }
    if (!r.sent || tex1 != r.tex1) {
        values[count] = tex1;
        names[count++] = GS_TEX1_1;
    }
    if (!r.sent || clamp != r.clamp) {
        values[count] = clamp;
        names[count++] = GS_CLAMP_1;
    }
    if (d.blend && (!r.sent || alpha != r.alpha)) {
        values[count] = alpha;
        names[count++] = GS_ALPHA_1;
        r.alpha = alpha;
    }
    if (!r.sent || test != r.test) {
        values[count] = test;
        names[count++] = GS_TEST_1;
    }
    if (!r.sent || zbuf != r.zbuf) {
        values[count] = zbuf;
        names[count++] = GS_ZBUF_1;
    }
    send_registers(r, values, names, count);
    r.tex0 = tex0;
    r.tex1 = tex1;
    r.clamp = clamp;
    r.test = test;
    r.zbuf = zbuf;
    r.sent = true;
}

/* a corner's color in the GS's terms: halved where a texture modulates it,
 * alpha always halved. its offset's three stay whole, of 255, and its
 * offset's alpha is the fog its points give it */
void color_of(uint32_t rgba, uint32_t offset, const Draw &d, Corner &c)
{
    bool halve = d.texture != nullptr && !d.decal;
    float scale = halve ? 128.0F / 255.0F : 1.0F;

    c.r = static_cast<float>(rgba & 0xff) * scale;
    c.g = static_cast<float>(rgba >> 8 & 0xff) * scale;
    c.b = static_cast<float>(rgba >> 16 & 0xff) * scale;
    c.a = static_cast<float>(rgba >> 24 & 0xff) * (128.0F / 255.0F);
    c.or_ = static_cast<float>(offset & 0xff);
    c.og = static_cast<float>(offset >> 8 & 0xff);
    c.ob = static_cast<float>(offset >> 16 & 0xff);
    c.fog = static_cast<float>(offset >> 24 & 0xff) * (1.0F / 255.0F);
}

/* a screen's terms: focal x and y, center x and y */
struct Screen {
    float fx, fy, cx, cy;
};

/* how far inside each of the five sides a view point lies */
void inside(const Screen &s, const Corner &c, float out[5])
{
    float x = s.fx * c.x + (s.cx - WIDTH / 2) * c.z;
    float y = s.fy * c.y + (s.cy - HEIGHT / 2) * c.z;
    float wide = REACH * (WIDTH / 2) * c.z, high = REACH * (HEIGHT / 2) * c.z;

    out[0] = c.z - NEAR_Z;
    out[1] = wide - x;
    out[2] = wide + x;
    out[3] = high - y;
    out[4] = high + y;
}

Corner between(const Corner &a, const Corner &b, float t)
{
    Corner m;

    m.x = a.x + (b.x - a.x) * t;
    m.y = a.y + (b.y - a.y) * t;
    m.z = a.z + (b.z - a.z) * t;
    m.u = a.u + (b.u - a.u) * t;
    m.v = a.v + (b.v - a.v) * t;
    m.r = a.r + (b.r - a.r) * t;
    m.g = a.g + (b.g - a.g) * t;
    m.b = a.b + (b.b - a.b) * t;
    m.a = a.a + (b.a - a.a) * t;
    m.or_ = a.or_ + (b.or_ - a.or_) * t;
    m.og = a.og + (b.og - a.og) * t;
    m.ob = a.ob + (b.ob - a.ob) * t;
    m.fog = a.fog + (b.fog - a.fog) * t;
    return m;
}

/* the fog table's fog for a corner at 1/z, of 1: 1/z times the density,
 * held to 1 to 256, picks an entry by its exponent and the mantissa's top
 * four bits, and the rest of the mantissa moves from the entry's high byte
 * to its low, as the PowerVR's pixel finds it */
float fog_alpha(const Fog &f, float q)
{
    float z = q * f.density;

    z = z < 1.0F ? 1.0F : z > 255.9999F ? 255.9999F : z;
    /* the float's own bits give the exponent and the mantissa's top four
     * bits, and the rest as the part between entries: the EE's library
     * takes far longer for a logarithm */
    uint32_t b = bits(z);
    int e = static_cast<int>(b >> 23 & 0xff) - 127;
    int index = e * 16 + static_cast<int>(b >> 19 & 15);
    float part = static_cast<float>(b & 0x7ffff) * (1.0F / 524288.0F);

    index = index < 0 ? 0 : index > 127 ? 127 : index;
    return (f.table[index][0] + (f.table[index][1] - f.table[index][0]) * part) * (1.0F / 255.0F);
}

/* the GS's fog for a corner, 255 for none: the table's by its 1/z, or the
 * corner's own under fog by its points */
uint8_t fog_of(const Held &r, const Draw &d, const Corner &v, float q)
{
    float alpha;

    switch (d.fog) {
    case SA_FOG_TABLE:
    case SA_FOG_TABLE2: {
        float z = q * r.fog.density;
        uint32_t b = bits(z < 1.0F ? 1.0F : z > 255.9999F ? 255.9999F : z);

        /* the exponent past 1.0's and the mantissa's top 7 bits */
        return r.fog.gs[(b - 0x3f800000U) >> 16];
    }
    case SA_FOG_VERTEX:
        alpha = v.fog;
        break;
    default:
        return 255;
    }
    alpha = alpha < 0.0F ? 0.0F : alpha > 1.0F ? 1.0F : alpha;
    return static_cast<uint8_t>(255.0F - alpha * 255.0F + 0.5F);
}

/* the corners as the GS takes them, in one packet: RGBAQ, then STQ where
 * textured, then XYZF2. the triangles' corners are pool's, by index, or in
 * pool's order where index is null. pass is 0 for the draw's own, or 1 for
 * a blend's offset, added over it untextured */
void send_packet(Held &r, const Draw &d, const Sent *pool, const uint32_t *index, uint32_t count, int pass)
{
    bool textured = d.texture != nullptr && pass == 0;
    bool fogged = d.fog != SA_FOG_NONE && pass == 0;
    bool high = highlights(d) && pass == 0;
    int regs = textured ? 3 : 2;
    uint64_t prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, textured ? 1 : 0, fogged ? 1 : 0,
                                   d.blend || pass != 0 ? 1 : 0, 0, 0, 0, 0);
    uint64_t names =
        textured ? (GS_RGBAQ | GS_ST << 4 | GS_XYZF2_REG << 8) : (GS_RGBAQ | GS_XYZF2_REG << 4);
    int offset_x = r.gs->OffsetX, offset_y = r.gs->OffsetY;

    if (count == 0) {
        return;
    }
    if (pass == 0) {
        apply(r, d);
    } else {
        /* the offset adds over the first pass's pixels, at their depth */
        Draw add = d;

        add.texture = nullptr;
        add.blend = true;
        add.src = SA_BLEND_SRC_ALPHA;
        add.dst = SA_BLEND_ONE;
        add.depth = SA_DEPTH_GEQUAL;
        add.depth_write = false;
        add.cut = false;
        apply(r, add);
    }
    /* the GIF's REGLIST form: a register's 64 bits each, half PACKED's
     * room. RGBAQ carries the corner's 1/z, which the GS takes as STQ's Q,
     * then ST, then XYZF2. a packet's loops are at most a GIF tag's 32767,
     * whole triangles, and its words an even count, so an odd one ends in
     * a word the GIF passes over */
    for (uint32_t first = 0; first < count;) {
        uint32_t take = std::min<uint32_t>(count - first, 3 * 4096);
        uint32_t words = take * static_cast<uint32_t>(regs);
        uint32_t qwords = (words + 1) / 2;

        set_prim(r, prim);
        u64 *p = gif(r, qwords + 1);

        *p++ = GIF_TAG(take, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, regs);
        *p++ = names;
        r.triangles += take / 3;
        for (uint32_t k = first; k < first + take; k++) {
            const Sent &s = pool[index != nullptr ? index[k] : k];
            const Corner &v = s.v;
            uint32_t red, green, blue, alpha;

            if (pass != 0) {
                /* the offset, faded as the first pass's fog fades it */
                float keep = d.fog != SA_FOG_NONE ? s.f * (1.0F / 255.0F) : 1.0F;

                red = static_cast<uint32_t>(v.or_ * keep);
                green = static_cast<uint32_t>(v.og * keep);
                blue = static_cast<uint32_t>(v.ob * keep);
                alpha = static_cast<uint32_t>(v.a);
            } else {
                red = static_cast<uint32_t>(v.r);
                green = static_cast<uint32_t>(v.g);
                blue = static_cast<uint32_t>(v.b);
                /* HIGHLIGHT2 adds the alpha to each of the three, and takes
                 * the texel's own alpha */
                alpha = high ? static_cast<uint32_t>(std::max(v.or_, std::max(v.og, v.ob)))
                             : static_cast<uint32_t>(v.a);
            }
            red = red > 255 ? 255 : red;
            green = green > 255 ? 255 : green;
            blue = blue > 255 ? 255 : blue;
            alpha = alpha > 255 ? 255 : alpha;
            *p++ = static_cast<u64>(red | green << 8 | blue << 16 | alpha << 24) | static_cast<u64>(bits(s.q)) << 32;
            if (textured) {
                *p++ = static_cast<u64>(bits(v.u * s.q)) | static_cast<u64>(bits(v.v * s.q)) << 32;
            }
            *p++ = static_cast<u64>(static_cast<uint32_t>(static_cast<int>(s.x * 16.0F) + offset_x) & 0xffff) |
                   static_cast<u64>(static_cast<uint32_t>(static_cast<int>(s.y * 16.0F) + offset_y) & 0xffff) << 16 |
                   (static_cast<u64>(static_cast<uint32_t>(s.z)) & 0xffffffU) << 32 |
                   static_cast<u64>(fogged ? s.f : 255) << 56;
        }
        if (words % 2 != 0) {
            *p++ = 0;
        }
        first += take;
    }
}

/* whether a blend's corners carry an offset to add */
bool lit_offset(const Draw &d, const Sent *pool, const uint32_t *index, uint32_t count)
{
    if (!d.offset || d.texture == nullptr || d.decal || highlights(d)) {
        return false;
    }
    for (uint32_t k = 0; k < count; k++) {
        const Corner &v = pool[index != nullptr ? index[k] : k].v;

        if (v.or_ + v.og + v.ob > 0.0F) {
            return true;
        }
    }
    return false;
}

/* a corner made for the GS from its corner in view space, or in the
 * picture's pixels where screen is set */
void make_sent(const Held &r, const Draw &d, const Screen &s, const Corner &v, bool screen, Sent &t)
{
    float q = screen ? 1.0F : 1.0F / v.z;

    t.v = v;
    t.q = q;
    /* what the screen's own pixels show lies on them whole: a picture of
     * tiles meets at a pixel's edge, where the GS's rule of what a
     * triangle covers leaves no line between two tiles */
    t.x = screen ? floorf(v.x + 0.5F) : s.cx + s.fx * v.x * q;
    t.y = screen ? floorf(v.y + 0.5F) : s.cy + s.fy * v.y * q - crop();
    t.z = screen ? (v.z > 1.0F ? DEPTH_MOST / v.z : DEPTH_MOST) : DEPTH_MOST * q;
    t.f = fog_of(r, d, v, screen ? (v.z > 1.0F ? 1.0F / v.z : 1.0F) : q);
}

bool same_place(const Corner &a, const Corner &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* the sides a corner lies outside, a bit each */
uint32_t outside(const Screen &s, const Corner &c)
{
    float in[5];
    uint32_t mask = 0;

    inside(s, c, in);
    for (int side = 0; side < 5; side++) {
        mask |= (in[side] < 0 ? 1U : 0U) << side;
    }
    return mask;
}

/* a draw's corners in view space, and its triangles by their indices,
 * three each, to the GS. each corner goes to view space, is tested against
 * the five sides and projected once, however many triangles share it. a
 * triangle within every side goes whole, one beyond a side goes not at all,
 * and else it is cut, its new corners made beside the draw's. culled as d
 * says, under s. screen is set for corners already in the picture's
 * pixels, whose z is the view's depth. a blend in the world waits for the
 * picture's sort */
void send_draw(Held &r, const Draw &d, const Screen &s, const Corner *corners, uint32_t count,
               const uint32_t *tri, uint32_t indices, bool screen)
{
    std::vector<Sent> &pool = r.sent_corners;
    std::vector<uint32_t> &masks = r.masks, &kept = r.kept_index;

    pool.resize(count);
    masks.resize(count);
    kept.clear();
    for (uint32_t i = 0; i < count; i++) {
        masks[i] = screen ? 0 : outside(s, corners[i]);
        /* a corner behind the eye isn't projected: its triangles are cut */
        if ((masks[i] & 1) == 0) {
            make_sent(r, d, s, corners[i], screen, pool[i]);
        }
    }
    for (uint32_t k = 0; k + 2 < indices; k += 3) {
        uint32_t a = tri[k], b = tri[k + 1], c = tri[k + 2];

        /* the walk joins a meshset's strips by triangles of a point twice,
         * which cover nothing */
        if (a >= count || b >= count || c >= count || (masks[a] & masks[b] & masks[c]) != 0 ||
            same_place(corners[a], corners[b]) || same_place(corners[b], corners[c]) ||
            same_place(corners[a], corners[c])) {
            continue;
        }
        uint32_t made[9];
        int n = 0;

        if ((masks[a] | masks[b] | masks[c]) == 0) {
            made[0] = a;
            made[1] = b;
            made[2] = c;
            n = 3;
        } else {
            Corner now[9], next[9];

            now[0] = corners[a];
            now[1] = corners[b];
            now[2] = corners[c];
            n = 3;
            for (int side = 0; side < 5 && n >= 3; side++) {
                int m = 0;

                for (int j = 0; j < n; j++) {
                    const Corner &p = now[j], &q = now[(j + 1) % n];
                    float ip[5], iq[5];

                    inside(s, p, ip);
                    inside(s, q, iq);
                    if (ip[side] >= 0 && m < 9) {
                        next[m++] = p;
                    }
                    if ((ip[side] >= 0) != (iq[side] >= 0) && m < 9) {
                        next[m++] = between(p, q, ip[side] / (ip[side] - iq[side]));
                    }
                }
                memcpy(now, next, static_cast<size_t>(m) * sizeof *now);
                n = m;
            }
            if (n < 3) {
                continue;
            }
            for (int j = 0; j < n; j++) {
                Sent t;

                make_sent(r, d, s, now[j], screen, t);
                made[j] = static_cast<uint32_t>(pool.size());
                pool.push_back(t);
            }
        }
        /* the polygon's fan: a whole triangle is its one */
        for (int j = 1; j + 1 < n; j++) {
            uint32_t t0 = made[0], t1 = made[j], t2 = made[j + 1];

            if (d.cull != 0) {
                const Sent &p0 = pool[t0], &p1 = pool[t1], &p2 = pool[t2];
                float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);

                if ((d.cull == 1 && area > 0) || (d.cull == 2 && area < 0)) {
                    continue;
                }
            }
            if (d.blend && !screen && r.picture) {
                if (r.blends.size() < BLENDS_MOST) {
                    Blend bl;

                    bl.d = d;
                    bl.first = static_cast<uint32_t>(r.blend_corners.size());
                    bl.key = depth_key((pool[t0].v.z + pool[t1].v.z + pool[t2].v.z) * (1.0F / 3.0F));
                    r.blend_corners.push_back(pool[t0]);
                    r.blend_corners.push_back(pool[t1]);
                    r.blend_corners.push_back(pool[t2]);
                    r.blends.push_back(bl);
                    continue;
                }
                r.blends_past++;
            }
            kept.push_back(t0);
            kept.push_back(t1);
            kept.push_back(t2);
        }
    }
    uint32_t left = static_cast<uint32_t>(kept.size());

    send_packet(r, d, pool.data(), kept.data(), left, 0);
    if (lit_offset(d, pool.data(), kept.data(), left)) {
        send_packet(r, d, pool.data(), kept.data(), left, 1);
    }
}

/* corners three to a triangle, in order */
void send_triangles(Held &r, const Draw &d, const Screen &s, const Corner *tri, uint32_t count, bool screen)
{
    std::vector<uint32_t> &order = r.order;

    if (order.size() < count) {
        uint32_t from = static_cast<uint32_t>(order.size());

        order.resize(count);
        for (uint32_t i = from; i < count; i++) {
            order[i] = i;
        }
    }
    send_draw(r, d, s, tri, count, order.data(), count - count % 3, screen);
}

/* the picture's waiting blends, the farthest first, each run of one state
 * as a packet */
void draw_blends(Held &r)
{
    std::vector<Blend> &b = r.blends;
    std::vector<uint32_t> &order = r.blend_order, &run = r.kept_index;
    static uint32_t counts[DEPTH_KEYS];
    uint32_t n = static_cast<uint32_t>(b.size());

    if (n == 0) {
        return;
    }
    r.most_blends = std::max<uint32_t>(r.most_blends, n);
    /* a counting sort by key, the farthest first: each key's count, then
     * the place each key starts at, the largest keys from 0, then the
     * triangles in their keys' places, each key's in the order they came */
    memset(counts, 0, sizeof counts);
    for (const Blend &bl : b) {
        counts[bl.key]++;
    }
    for (uint32_t k = DEPTH_KEYS, at = 0; k-- > 0;) {
        uint32_t here = counts[k];

        counts[k] = at;
        at += here;
    }
    order.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        order[counts[b[i].key]++] = i;
    }
    for (uint32_t i = 0; i < n;) {
        const Draw &d = b[order[i]].d;
        uint32_t j = i;

        run.clear();
        while (j < n && b[order[j]].d.same(d)) {
            uint32_t first = b[order[j]].first;

            run.push_back(first);
            run.push_back(first + 1);
            run.push_back(first + 2);
            j++;
        }
        uint32_t corners = static_cast<uint32_t>(run.size());

        send_packet(r, d, r.blend_corners.data(), run.data(), corners, 0);
        if (lit_offset(d, r.blend_corners.data(), run.data(), corners)) {
            send_packet(r, d, r.blend_corners.data(), run.data(), corners, 1);
        }
        i = j;
    }
    b.clear();
    r.blend_corners.clear();
}

/* a full-screen rectangle of color at depth z, under a depth test */
void fill(Held &r, uint32_t argb, uint32_t z, uint64_t ztst, bool zwrite)
{
    uint64_t values[2] = {GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, ztst),
                          GS_SETREG_ZBUF(r.gs->ZBuffer / 8192, r.gs->PSMZ, zwrite ? 0 : 1)};
    uint64_t names[2] = {GS_TEST_1, GS_ZBUF_1};

    send_registers(r, values, names, 2);
    r.test = values[0];
    r.zbuf = values[1];
    u64 *p = gif(r, 4);

    p[0] = GIF_TAG(1, 1, 1, GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), GSKIT_GIF_FLG_PACKED,
                   3);
    p[1] = GS_RGBAQ | GS_XYZ2 << 4 | GS_XYZ2 << 8;
    p[2] = static_cast<u64>(argb >> 16 & 0xff) | static_cast<u64>(argb >> 8 & 0xff) << 32;
    p[3] = static_cast<u64>(argb & 0xff) | static_cast<u64>(0x80) << 32;
    p[4] = static_cast<u64>(gsKit_float_to_int_x(r.gs, 0)) | static_cast<u64>(gsKit_float_to_int_y(r.gs, 0)) << 32;
    p[5] = z;
    p[6] = static_cast<u64>(gsKit_float_to_int_x(r.gs, SCREEN_W)) |
           static_cast<u64>(gsKit_float_to_int_y(r.gs, screen_height())) << 32;
    p[7] = z;
}

/* 480i's flicker filter. a line of an interlaced picture shows in one
 * field of two, so a thin line or an edge flickers at 30 Hz. the GS's
 * second read circuit shows the picture just shown again, a line lower,
 * and the merge circuit shows each pixel half the one and half the other:
 * each field's line takes half of the line below it, the other field's.
 * gsKit drives the first circuit's twin, read circuit 2, at each flip, from
 * the buffer it showed before the flip's toggle, so the filter follows it
 * there after each */
void show_buffer(GSGLOBAL *gs, uint32_t buffer)
{
    uint32_t at = gs->ScreenBuffer[buffer & 1];

    GS_SET_DISPFB2(at / 8192, gs->Width / 64, gs->PSM, 0, 0);
    if (ps2_interlaced && ps2_deflicker) {
        GS_SET_DISPFB1(at / 8192, gs->Width / 64, gs->PSM, 0, 1);
        GS_SET_DISPLAY1(gs->StartX + gs->StartXOffset, gs->StartY + gs->StartYOffset, gs->MagH, gs->MagV,
                        gs->DW - 1, gs->DH - 1);
        /* both circuits on, merged by the fixed alpha, half of each */
        GS_SET_PMODE(1, 1, 1, 0, 0, 0x80);
    }
}

/* the flip, at the vertical blank, by its interrupt: a picture's commands
 * go to the GS at the picture's end, and the buffer they draw shows at the
 * next blank, while the EE goes on to the next picture. it draws into the
 * other buffer, which no command reaches before that picture's end. that
 * picture's end waits for the flip only where it comes before the blank,
 * so the EE waits for the display only when it draws faster than it shows.
 * the GS's display circuits show from the blank, so no picture tears */
GSGLOBAL *flip_gs;

int flip_at_blank(int)
{
    /* the GS's FINISH says it drew the picture's last command */
    if (flip_pending >= 0 && flip_gs != nullptr && (*GS_CSR & 2) != 0) {
        show_buffer(flip_gs, static_cast<uint32_t>(flip_pending));
        flip_pending = -1;
    }
    ExitHandler();
    return 0;
}

/* VU1's program (engine/ps2/strip.vsm), and its two batches' buffers, by
 * a transfer of their own before the first picture's */
void open_vu(Held &r)
{
    /* each program, and where in VU1's code memory it lies, in
     * instructions */
    const struct {
        const uint32_t *start, *end;
        uint32_t at;
    } programs[] = {{&VU1Strip_CodeStart, &VU1Strip_CodeEnd, 0},
                    {&VU1Model_CodeStart, &VU1Model_CodeEnd, VU_MODEL_AT}};
    uint32_t *w = r.stream[0];
    uint32_t at = 0;

    for (const auto &program : programs) {
        uint32_t dwords = static_cast<uint32_t>(program.end - program.start) / 2;

        if (program.at != 0 && dwords > VU_MODEL_AT) {
            fprintf(stderr, "sa1: VU1's strip program passes the model program's place\n");
        }
        for (uint32_t done = 0; done < dwords;) {
            uint32_t n = std::min<uint32_t>(dwords - done, 256);

            /* MPG's code starts at a doubleword's start */
            while (at % 2 != 1) {
                w[at++] = 0;
            }
            w[at++] = vif_code(VIF_MPG, n & 0xff, program.at + done);
            memcpy(w + at, program.start + done * 2, n * 8);
            at += n * 2;
            done += n;
        }
    }
    w[at++] = vif_code(VIF_BASE, 0, VU_BASE);
    w[at++] = vif_code(VIF_OFFSET, 0, VU_OFFSET);
    w[at++] = vif_code(VIF_STCYCL, 0, 1 | 1 << 8);
    while (at % 4 != 0) {
        w[at++] = 0;
    }
    FlushCache(0);
    *D1_MADR = physical(w);
    *D1_QWC = at / 4;
    *D1_CHCR = 0x101; /* from memory, at once */
    vif_wait();
}

void open_screen(Held &r)
{
    GSGLOBAL *gs;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
                1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    /* gsKit's queue takes only its own setup's commands */
    gs = gsKit_init_global_custom(1 << 16, GS_RENDER_QUEUE_PER_POOLSIZE);
    /* 480p, or 480i: the game's pixels map one to one either way, 480i's
     * cropped to its 448 lines (crop) */
    gs->Mode = ps2_interlaced ? GS_MODE_NTSC : GS_MODE_DTV_480P;
    gs->Interlace = ps2_interlaced ? GS_INTERLACED : GS_NONINTERLACED;
    /* 480i's field mode reads every other line of one whole picture, each
     * field its own: its frame mode would read a picture of half the
     * lines for each field */
    gs->Field = ps2_interlaced ? GS_FIELD : GS_FRAME;
    gs->Width = SCREEN_W;
    gs->Height = screen_height();
    gs->PSM = GS_PSM_CT16S;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_ON;
    /* the screen holds 5 bits of each of the three: the dither spreads the
     * rest over a 4 by 4 pattern, which a CRT blurs and a sharp display
     * shows. off, a gradient bands instead */
    gs->Dithering = ps2_dither ? GS_SETTING_ON : GS_SETTING_OFF;
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_queue_exec(gs);
    dmaKit_wait(DMA_CHANNEL_GIF, 0);
    /* the first picture draws into buffer 0 while 1 shows. gsKit's own
     * flip, which waits for the blank, goes unused */
    r.draw_buffer = 0;
    show_buffer(gs, 1);
    flip_gs = gs;
    gsKit_add_vsync_handler(flip_at_blank);
    r.gs = gs;
    r.vram_first = (gs->CurrentPointer + 255) / 256;
    r.kept = static_cast<psp_corner *>(memalign(64, KEPT_CORNERS * sizeof(psp_corner)));
    r.stream[0] = static_cast<uint32_t *>(memalign(64, STREAM_QWORDS * 16));
    r.stream[1] = static_cast<uint32_t *>(memalign(64, STREAM_QWORDS * 16));
    r.blends.reserve(BLENDS_MOST);
    r.blend_corners.reserve(3 * BLENDS_MOST);
    r.opened = r.kept != nullptr && r.stream[0] != nullptr && r.stream[1] != nullptr;
    if (r.opened) {
        open_vu(r);
    }
    fprintf(stderr, "sa1: the GS's screen is %dx%d, %s, %u bytes of its memory free for textures\n", SCREEN_W,
            screen_height(), ps2_interlaced ? (ps2_deflicker ? "480i, its flicker filtered" : "480i") : "480p",
            4U * 1024 * 1024 - gs->CurrentPointer);
}

/* the PowerVR's register at address, as Ninja's setters wrote it */
uint32_t pvr_register(uint32_t address)
{
    uint32_t word;

    memcpy(&word, console_registers + CONSOLE_PVR + address, sizeof word);
    return word;
}

/* the picture's fog from the PowerVR's registers (engine/sa1/records.c's
 * begin_picture reads them so) */
void read_fog(Held &r)
{
    uint32_t density = pvr_register(FOG_DENSITY);
    int mantissa = static_cast<int>(density >> 8 & 0xff);
    int exponent = static_cast<int8_t>(density & 0xff);

    r.fog.density = static_cast<float>(mantissa) / 128.0F * ldexpf(1.0F, exponent);
    r.fog.color = pvr_register(FOG_COLOR);
    for (int i = 0; i < 128; i++) {
        uint32_t entry = pvr_register(FOG_TABLE + 4 * static_cast<uint32_t>(i));

        r.fog.table[i][0] = static_cast<uint8_t>(entry >> 8);
        r.fog.table[i][1] = static_cast<uint8_t>(entry);
    }
    /* the GS's fog at each step's middle, as fog_alpha finds the PowerVR's
     * for that 1/z times the density */
    for (uint32_t i = 0; i < sizeof r.fog.gs; i++) {
        float z;
        uint32_t b = 0x3f800000U + (i << 16) + (1U << 15);

        memcpy(&z, &b, sizeof z);
        float alpha = fog_alpha(r.fog, r.fog.density != 0.0F ? z / r.fog.density : 0.0F);

        alpha = alpha < 0.0F ? 0.0F : alpha > 1.0F ? 1.0F : alpha;
        r.fog.gs[i] = static_cast<uint8_t>(255.0F - alpha * 255.0F + 0.5F);
    }
}

/* take back the open picture's unsent commands: what they would have sent
 * of a texture's texels goes unsent, so its next bind sends it */
void take_back(Held &r)
{
    r.taken_back++;
    r.at = 0;
    r.tag = -1;
    r.direct = -1;
    r.model_open = false;
    r.vu_palette = UINT32_MAX;
    for (Texture *t : r.uploads) {
        unplace(r, t);
    }
    r.uploads.clear();
    r.bound = nullptr;
    r.sent = false;
    r.vu_fog = false;
}

/* open a picture's commands over a screen cleared to black and the
 * farthest depth, under the picture's fog color */
void open_picture(Held &r)
{
    if (r.picture) {
        take_back(r);
    }
    r.sent = false;
    r.bound = nullptr;
    r.vu_fog = false;
    /* the palettes may have changed since the last picture */
    r.vu_palette = UINT32_MAX;
    read_fog(r);
    {
        /* the alpha bits of a 16-bit texel, clear and solid, and the fog's
         * color, the PowerVR's ARGB as the GS's RGB */
        uint32_t c = r.fog.color;
        /* and the buffer the picture draws into, the one not showing */
        uint64_t values[3] = {static_cast<uint64_t>(0x80) << 32,
                              static_cast<uint64_t>(c >> 16 & 0xff) | (c >> 8 & 0xff) << 8 | (c & 0xff) << 16,
                              GS_SETREG_FRAME(r.gs->ScreenBuffer[r.draw_buffer & 1] / 8192, r.gs->Width / 64,
                                              r.gs->PSM, 0)};
        uint64_t names[3] = {GS_TEXA, GS_FOGCOL_REG, GS_FRAME_1};

        send_registers(r, values, names, 3);
    }
    /* the picture's color, the last one the game gave, at the farthest
     * depth: what draws no depth, a sky among them, then stays over it */
    fill(r, r.clear, 0, 1, true);
    r.picture = true;
    r.made_since = sceKernelGetSystemTimeLow();
    r.picture_triangles = r.triangles;
    r.picture_sends = r.sends;
    r.picture_load_us = ps2_walk_load_us;
    r.picture_strip_us = r.strip_us;
    r.picture_bind_us = r.bind_us;
    r.place_kept = false;
    r.places.clear();
    r.later.clear();
    r.blends.clear();
    r.blend_corners.clear();
    r.kept_used = 0;
}

/* the copy of t mirrored past 1 as flip says, 1 for u, 2 for v, 3 for both:
 * twice its side each way it mirrors, its first half t's and its second
 * t's again turned. a copy past the GS's longest side mirrors none */
Texture *mirrored(Held &r, Texture *t, int flip)
{
    if (flip == 0 || t == nullptr) {
        return t;
    }
    if (t->mirrored[flip - 1] != nullptr) {
        return t->mirrored[flip - 1];
    }
    bool mu = (flip & 1) != 0, mv = (flip & 2) != 0;
    uint32_t pw = t->gs.Width, ph = t->gs.Height;
    uint32_t w2 = mu ? pw * 2 : pw, h2 = mv ? ph * 2 : ph;
    uint32_t bits = texel_bits(t->gs.PSM);

    /* a large texture's copy would take more of the EE's memory than its
     * mirror is worth: it repeats instead */
    if (w2 > SIDE_MOST || h2 > SIDE_MOST || pw > MIRROR_MOST || ph > MIRROR_MOST) {
        return t;
    }
    uint8_t *texels = static_cast<uint8_t *>(memalign(128, w2 * h2 * bits / 8));

    if (texels == nullptr) {
        return t;
    }
    const uint8_t *from = reinterpret_cast<const uint8_t *>(t->gs.Mem);

    memset(texels, 0, w2 * h2 * bits / 8);
    for (uint32_t y = 0; y < h2; y++) {
        uint32_t sy = y < ph ? y : 2 * ph - 1 - y;

        for (uint32_t x = 0; x < w2; x++) {
            uint32_t sx = x < pw ? x : 2 * pw - 1 - x;
            uint32_t to = y * w2 + x, at = sy * pw + sx;

            if (bits == 4) {
                /* two to a byte, the first in the low half */
                texels[to / 2] |= static_cast<uint8_t>((from[at / 2] >> (4 * (at & 1)) & 15) << (4 * (to & 1)));
            } else {
                memcpy(texels + to * bits / 8, from + at * bits / 8, bits / 8);
            }
        }
    }
    Texture *made = new Texture;

    made->gs = t->gs;
    made->gs.Width = w2;
    made->gs.Height = h2;
    made->gs.TBW = tbw_of(w2, t->gs.PSM);
    made->gs.Mem = reinterpret_cast<u32 *>(texels);
    made->gs.Vram = 0;
    /* the copy's own texels, and its texture's CLUT, which outlives it */
    made->clut = t->clut;
    made->clut_colors = t->clut_colors;
    made->width = t->width;
    made->height = t->height;
    /* a texel's place over the copy's sides: u in 0 to 2 spans the copy */
    made->su = mu ? t->su * 0.5F : t->su;
    made->sv = mv ? t->sv * 0.5F : t->sv;
    made->tw = static_cast<uint8_t>(side_log2(w2));
    made->th = static_cast<uint8_t>(side_log2(h2));
    made->cuts = t->cuts;
    size_texture(made);
    FlushCache(0);
    r.texture_bytes += w2 * h2 * bits / 8;
    r.mirrors++;
    t->mirrored[flip - 1] = made;
    return made;
}

Draw draw_of(Held &r, const psp_state &s)
{
    Draw d;
    Texture *t = s.texture != 0 && s.texture <= r.textures.size() ? r.textures[s.texture - 1] : nullptr;

    d.texture = mirrored(r, t, (s.flip_u ? 1 : 0) | (s.flip_v ? 2 : 0));
    d.decal = s.decal != 0;
    d.alpha = s.cut != 0 || s.blend != 0;
    d.clamp_u = s.clamp_u != 0;
    d.clamp_v = s.clamp_v != 0;
    d.linear = s.point == 0;
    d.cut = s.cut != 0;
    d.blend = s.blend != 0;
    d.src = s.src;
    d.dst = s.dst;
    d.depth = s.depth;
    d.depth_write = s.depth_write != 0;
    d.cull = s.cull;
    d.offset = s.offset != 0;
    d.fog = s.fog & 3;
    return d;
}

/* an opaque strip as the GS's own triangle strip, where each of its corners
 * lies within every side: each corner projected and written once, and the
 * GS makes the strip's triangles, those the walk joins its strips by among
 * them, which cover nothing. the GS culls none, so a single-sided strip
 * shows its back too, where the depth of what faces the eye hides it. 0
 * where a corner lies past a side, and the strip goes as triangles */
bool send_strip(Held &r, const Draw &d, const Screen &s, const Corner *corners, uint32_t count)
{
    const uint32_t most = 3 * 1024;
    bool textured = d.texture != nullptr;
    bool fogged = d.fog != SA_FOG_NONE;
    bool high = highlights(d);
    int regs = textured ? 3 : 2;
    int offset_x = r.gs->OffsetX, offset_y = r.gs->OffsetY;

    if (count < 3 || count > most) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (outside(s, corners[i]) != 0) {
            return false;
        }
    }
    uint32_t words = count * static_cast<uint32_t>(regs);
    uint32_t qwords = (words + 1) / 2;

    apply(r, d);
    set_prim(r, GS_SETREG_PRIM(GS_PRIM_PRIM_TRISTRIP, 1, textured ? 1 : 0, fogged ? 1 : 0, 0, 0, 0, 0, 0));
    u64 *p = gif(r, qwords + 1);

    *p++ = GIF_TAG(count, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, regs);
    *p++ = textured ? (GS_RGBAQ | GS_ST << 4 | GS_XYZF2_REG << 8) : (GS_RGBAQ | GS_XYZF2_REG << 4);
    r.triangles += count - 2;
    for (uint32_t i = 0; i < count; i++) {
        const Corner &v = corners[i];
        float q = 1.0F / v.z;
        float x = s.cx + s.fx * v.x * q;
        float y = s.cy + s.fy * v.y * q - crop();
        uint32_t f = fogged ? fog_of(r, d, v, q) : 255;
        uint32_t red = static_cast<uint32_t>(v.r), green = static_cast<uint32_t>(v.g);
        uint32_t blue = static_cast<uint32_t>(v.b);
        uint32_t alpha = high ? static_cast<uint32_t>(std::max(v.or_, std::max(v.og, v.ob))) : static_cast<uint32_t>(v.a);

        red = red > 255 ? 255 : red;
        green = green > 255 ? 255 : green;
        blue = blue > 255 ? 255 : blue;
        alpha = alpha > 255 ? 255 : alpha;
        *p++ = static_cast<u64>(red | green << 8 | blue << 16 | alpha << 24) | static_cast<u64>(bits(q)) << 32;
        if (textured) {
            *p++ = static_cast<u64>(bits(v.u * q)) | static_cast<u64>(bits(v.v * q)) << 32;
        }
        *p++ = static_cast<u64>(static_cast<uint32_t>(static_cast<int>(x * 16.0F) + offset_x) & 0xffff) |
               static_cast<u64>(static_cast<uint32_t>(static_cast<int>(y * 16.0F) + offset_y) & 0xffff) << 16 |
               (static_cast<u64>(static_cast<uint32_t>(DEPTH_MOST * q)) & 0xffffffU) << 32 |
               static_cast<u64>(f) << 56;
    }
    if (words % 2 != 0) {
        *p++ = 0;
    }
    return true;
}

/* whether every point of a strip lies within every side, by the eight
 * corners of the box of its points in the model's space: each side is a
 * plane, so what lies between corners within lies within */
/* the EE's FPU's own least and most of two, without a branch */
inline float least(float a, float b)
{
    float m;

    __asm__("min.s %0, %1, %2" : "=f"(m) : "f"(a), "f"(b));
    return m;
}

inline float most(float a, float b)
{
    float m;

    __asm__("max.s %0, %1, %2" : "=f"(m) : "f"(a), "f"(b));
    return m;
}

static_assert(sizeof(psp_corner) == 24, "send_vu reads a corner as six words");

/* where a strip's box lies: within every side, past one side whole, or
 * across a side */
enum class Box { INSIDE, OUTSIDE, ACROSS };

Box box_of(const Placed &placed, const Screen &s, const psp_corner *corners, uint32_t count)
{
    const float *m = placed.matrix;
    float lo[3] = {corners[0].x, corners[0].y, corners[0].z};
    float hi[3] = {lo[0], lo[1], lo[2]};

    for (uint32_t i = 1; i < count; i++) {
        const psp_corner &c = corners[i];

        lo[0] = least(lo[0], c.x);
        hi[0] = most(hi[0], c.x);
        lo[1] = least(lo[1], c.y);
        hi[1] = most(hi[1], c.y);
        lo[2] = least(lo[2], c.z);
        hi[2] = most(hi[2], c.z);
    }
    /* the sides every corner lies past, and those any does */
    uint32_t every = 31, any = 0;

    for (int k = 0; k < 8; k++) {
        float x = (k & 1) != 0 ? hi[0] : lo[0];
        float y = (k & 2) != 0 ? hi[1] : lo[1];
        float z = (k & 4) != 0 ? hi[2] : lo[2];
        Corner v;

        v.x = x * m[0] + y * m[4] + z * m[8] + m[12];
        v.y = x * m[1] + y * m[5] + z * m[9] + m[13];
        v.z = x * m[2] + y * m[6] + z * m[10] + m[14];
        uint32_t past = outside(s, v);

        every &= past;
        any |= past;
    }
    return every != 0 ? Box::OUTSIDE : any != 0 ? Box::ACROSS : Box::INSIDE;
}

/* the picture's fog table to VU1, once its batches before are done: for
 * each step of the square root of 1/z times the density, the GS's fog at
 * the step's middle */
void send_vu_fog(Held &r)
{
    room(r, 8 + VU_FOG * 4);
    close_direct(r);
    put(r, vif_code(VIF_FLUSHE, 0, 0));
    while (r.at % 4 != 3) {
        put(r, 0);
    }
    put(r, vif_code(UNPACK_V4_32, VU_FOG, 0));
    float *f = reinterpret_cast<float *>(words(r) + r.at);

    for (uint32_t i = 0; i < VU_FOG; i++) {
        float root = (static_cast<float>(i) + 0.5F) * 0.25F;
        float alpha = fog_alpha(r.fog, r.fog.density != 0.0F ? root * root / r.fog.density : 0.0F);

        alpha = alpha < 0.0F ? 0.0F : alpha > 1.0F ? 1.0F : alpha;
        f[4 * i] = 255.0F - alpha * 255.0F;
        f[4 * i + 1] = f[4 * i + 2] = f[4 * i + 3] = 0.0F;
    }
    r.at += VU_FOG * 4;
    r.vu_fog = true;
}

/* an opaque strip within every side, by VU1: batches of VU_CORNERS
 * corners at most, each after the last's last two, each with its head of
 * the place, the screen and the draw's terms (engine/ps2/strip.vsm). the
 * VIF unpacks each corner's point, its u and v, its color and its offset
 * as the corner holds them, four qwords apart. VU1 culls as d says, each
 * triangle by the strip's first's turn, or the other where reversed: a
 * batch starts at an even triangle, so each batch's first turns as the
 * strip's */
void send_vu(Held &r, const Draw &d, const Placed &placed, const psp_corner *corners, uint32_t count, bool reversed)
{
    float turn = d.cull == 1 ? 1.0F : d.cull == 2 ? -1.0F : 0.0F;

    bool textured = d.texture != nullptr, fogged = d.fog != SA_FOG_NONE, high = highlights(d);
    float scale = textured && !d.decal ? 128.0F / 255.0F : 1.0F;
    float su = textured ? d.texture->su / 255.0F : 0.0F, sv = textured ? d.texture->sv / 255.0F : 0.0F;
    uint64_t prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRISTRIP, 1, textured ? 1 : 0, fogged ? 1 : 0, 0, 0, 0, 0, 0);
    uint64_t regs = GS_ST | GS_RGBAQ << 4 | GS_XYZF2_REG << 8;

    apply(r, d);
    if (fogged && !r.vu_fog) {
        send_vu_fog(r);
    }
    for (uint32_t first = 0; first + 2 < count; first += VU_CORNERS - 2) {
        uint32_t n = std::min(count - first, VU_CORNERS);
        const psp_corner *c = corners + first;

        room(r, 64 + 6 * n);
        close_direct(r);
        while (r.at % 4 != 3) {
            put(r, 0);
        }
        put(r, vif_code(UNPACK_V4_32, 12, UNPACK_TOPS));
        uint32_t *w = words(r) + r.at;
        u64 *tag = reinterpret_cast<u64 *>(w);
        float *h = reinterpret_cast<float *>(w);

        tag[0] = GIF_TAG(n, 1, 1, prim, GSKIT_GIF_FLG_PACKED, 3);
        tag[1] = regs;
        w[4] = n;
        h[5] = 0.0F;
        h[6] = reversed ? -turn : turn;
        h[7] = 2048.0F;
        memcpy(h + 8, placed.matrix, 16 * sizeof(float));
        h[24] = placed.screen[0];
        h[25] = placed.screen[1];
        h[26] = DEPTH_MOST;
        h[27] = 0.0F;
        h[28] = placed.screen[2] + static_cast<float>(r.gs->OffsetX) / 16.0F;
        h[29] = placed.screen[3] - crop() + static_cast<float>(r.gs->OffsetY) / 16.0F;
        h[30] = h[31] = 0.0F;
        h[32] = h[33] = h[34] = scale;
        h[35] = 128.0F / 255.0F;
        h[36] = su;
        h[37] = sv;
        h[38] = h[39] = 0.0F;
        h[40] = r.fog.density;
        h[41] = fogged ? 1.0F : 0.0F;
        h[42] = 1e30F;
        h[43] = high ? 1.0F : 0.0F;
        h[44] = 4.0F;
        h[45] = 255.99F;
        h[46] = fogged ? 0.0F : 255.0F;
        h[47] = high ? 0.0F : 1.0F;
        r.at += 48;
        put(r, vif_code(VIF_STCYCL, 0, 4 | 1 << 8));
        {
            /* the four fields' unpacks, each its code and its n words or
             * 3n, written in one pass over the corners: a corner's words
             * are u and v, the color, the point's three and the offset */
            typedef uint32_t word __attribute__((may_alias));
            word *out = words(r);
            const word *from = reinterpret_cast<const word *>(c);
            uint32_t at = r.at;
            uint32_t points = at + 1, uvs = points + 3 * n + 1, colors = uvs + n + 1, offsets = colors + n + 1;

            out[at] = vif_code(UNPACK_V3_32, n, UNPACK_TOPS | 12);
            out[uvs - 1] = vif_code(UNPACK_V2_16, n, UNPACK_TOPS | 13);
            out[colors - 1] = vif_code(UNPACK_V4_8, n, UNPACK_TOPS | UNPACK_USN | 14);
            out[offsets - 1] = vif_code(UNPACK_V4_8, n, UNPACK_TOPS | UNPACK_USN | 15);
            for (uint32_t i = 0; i < n; i++, from += 6) {
                uint32_t uv = from[0], color = from[1], x = from[2], y = from[3], z = from[4], offset = from[5];

                out[points + 3 * i] = x;
                out[points + 3 * i + 1] = y;
                out[points + 3 * i + 2] = z;
                out[uvs + i] = uv;
                out[colors + i] = color;
                out[offsets + i] = offset;
            }
            r.at = offsets + n;
        }
        put(r, vif_code(VIF_STCYCL, 0, 1 | 1 << 8));
        put(r, vif_code(VIF_MSCAL, 0, 0));
        r.vu_since = true;
        r.triangles += n - 2;
        r.vu_triangles += n - 2;
        r.vu_batches++;
        if (first + n >= count) {
            break;
        }
    }
}

/* a strip whose box lies wholly past a side draws nothing, and an opaque
 * strip within every side goes to VU1, but under fog by its points, whose
 * fog VU1's program leaves out. false where the EE must draw it */
bool vu_strip(Held &r, const Placed &placed, const Draw &d, const psp_corner *corners, uint32_t count,
              bool reversed)
{
    Screen s = {placed.screen[0], placed.screen[1], placed.screen[2], placed.screen[3]};

    if (!ps2_vu || count < 3) {
        return false;
    }
    Box box = box_of(placed, s, corners, count);

    if (box == Box::OUTSIDE) {
        r.strips_past++;
        return true;
    }
    if (box == Box::ACROSS || d.blend || d.fog == SA_FOG_VERTEX) {
        return false;
    }
    send_vu(r, d, placed, corners, count, reversed);
    return true;
}

/* a strip's or a list's corners under a place and screen, to view space and
 * the GS. each corner goes to view space once, and the triangles name them
 * by index: a strip's turn by turns, so each odd one goes reversed and
 * every triangle turns as the strip's first */
void send_corners(Held &r, const Placed &placed, const psp_state &state, const psp_corner *corners,
                  uint32_t count, bool triangles)
{
    const float *m = placed.matrix;
    Screen s = {placed.screen[0], placed.screen[1], placed.screen[2], placed.screen[3]};
    Draw d = draw_of(r, state);
    std::vector<Corner> &in = r.cut_in;
    std::vector<uint32_t> &tri = r.strip_index;
    float su = d.texture != nullptr ? d.texture->su / 255.0F : 0.0F;
    float sv = d.texture != nullptr ? d.texture->sv / 255.0F : 0.0F;

    if (count < 3) {
        return;
    }
    uint32_t began = stamp();

    if (!triangles && vu_strip(r, placed, d, corners, count, false)) {
        r.strip_us += stamp() - began;
        return;
    }
    in.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const psp_corner &p = corners[i];
        Corner &c = in[i];

        /* Ninja's matrix takes a point as a row */
        c.x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
        c.y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
        c.z = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
        c.u = p.u * su;
        c.v = p.v * sv;
        color_of(p.color, p.offset, d, c);
    }
    /* an opaque strip within every side goes as the GS's own strip */
    if (!triangles && !d.blend && send_strip(r, d, s, in.data(), count)) {
        r.strip_us += stamp() - began;
        return;
    }
    tri.clear();
    if (triangles) {
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            tri.push_back(i);
            tri.push_back(i + 1);
            tri.push_back(i + 2);
        }
    } else {
        for (uint32_t i = 0; i + 2 < count; i++) {
            bool odd = (i & 1) != 0;

            tri.push_back(odd ? i + 1 : i);
            tri.push_back(odd ? i : i + 1);
            tri.push_back(i + 2);
        }
    }
    send_draw(r, d, s, in.data(), count, tri.data(), static_cast<uint32_t>(tri.size()), false);
    r.strip_us += stamp() - began;
}

/* the strips the walk kept: they blend, so each goes to the picture's sort */
void draw_later(Held &r)
{
    for (const Later &l : r.later) {
        send_corners(r, r.places[l.place], l.state, r.kept + l.first, l.count, l.triangles);
    }
    r.later.clear();
    r.kept_used = 0;
}

void keep_place(Held &r)
{
    if (!r.place_kept) {
        r.places.push_back(r.place);
        r.place_kept = true;
    }
}

uint32_t keep_texture(Held &r, Texture *made);

/* a texture of texels laid out as head says (engine/ps2/texels.h), which
 * the raster keeps and frees with the texture: from the pack, or made here
 * of an image */
uint32_t add_texels(Held &r, const ps2::TexelsHead &head, void *texels)
{
    uint32_t pw = 1U << head.tw, ph = 1U << head.th;

    if (!r.opened || texels == nullptr) {
        return 0;
    }
    Texture *made = new Texture;

    made->gs.Width = pw;
    made->gs.Height = ph;
    made->gs.PSM = head.psm == ps2::PSM_32 ? GS_PSM_CT32 : GS_PSM_CT16;
    made->gs.TBW = std::max<uint32_t>(1, pw / 64);
    made->gs.Mem = static_cast<u32 *>(texels);
    made->gs.Filter = GS_FILTER_LINEAR;
    made->width = head.width;
    made->height = head.height;
    made->su = static_cast<float>(head.width) / pw;
    made->sv = static_cast<float>(head.height) / ph;
    made->tw = head.tw;
    made->th = head.th;
    made->cuts = head.cuts != 0;
    size_texture(made);
    if (head.bytes > 512 * 1024) {
        fprintf(stderr, "sa1: a large texture, %ux%u at %u bits: %u bytes\n", static_cast<unsigned>(head.width),
                static_cast<unsigned>(head.height), head.psm == ps2::PSM_32 ? 32U : 16U,
                static_cast<unsigned>(head.bytes));
    }
    FlushCache(0);
    r.texture_bytes += head.bytes;
    return keep_texture(r, made);
}

/* a picture of a TIM2's whose texels and CLUT stay in its lender's memory
 * (textures.cpp): the texture gives the lender back at its drop */
uint32_t add_picture(Held &r, const ps2::Lent &lent)
{
    if (!r.opened || lent.texels == nullptr) {
        return 0;
    }
    Texture *made = new Texture;

    made->gs.Width = lent.side_w;
    made->gs.Height = lent.side_h;
    made->gs.PSM = lent.psm;
    made->gs.TBW = tbw_of(lent.side_w, lent.psm);
    made->gs.Mem = static_cast<u32 *>(const_cast<void *>(lent.texels));
    made->gs.Filter = GS_FILTER_LINEAR;
    made->width = lent.width;
    made->height = lent.height;
    made->su = static_cast<float>(lent.width) / lent.side_w;
    made->sv = static_cast<float>(lent.height) / lent.side_h;
    made->tw = static_cast<uint8_t>(side_log2(lent.side_w));
    made->th = static_cast<uint8_t>(side_log2(lent.side_h));
    made->cuts = lent.cuts;
    made->clut = lent.clut;
    made->clut_colors = lent.clut_colors;
    made->lender = lent.lender;
    made->fixed = lent.fixed;
    size_texture(made);
    r.texture_bytes += lent.side_w * lent.side_h * texel_bits(lent.psm) / 8;
    return keep_texture(r, made);
}

/* a texture made, by its number, 1 and up */
uint32_t keep_texture(Held &r, Texture *made)
{
    for (size_t i = 0; i < r.textures.size(); i++) {
        if (r.textures[i] == nullptr) {
            r.textures[i] = made;
            return static_cast<uint32_t>(i + 1);
        }
    }
    r.textures.push_back(made);
    r.most_textures = std::max<uint32_t>(r.most_textures, static_cast<uint32_t>(r.textures.size()));
    return static_cast<uint32_t>(r.textures.size());
}

uint32_t add_image(Held &r, const Image &image)
{
    ps2::TexelsHead head;
    std::vector<uint8_t> &made = r.made_texels;

    if (!r.opened || !ps2::texels_of(image, head, made)) {
        return 0;
    }
    void *texels = memalign(128, head.bytes);

    if (texels == nullptr) {
        return 0;
    }
    memcpy(texels, made.data(), head.bytes);
    uint32_t added = add_texels(r, head, texels);

    if (added == 0) {
        free(texels);
    }
    return added;
}

/* a texture dropped: a stream unsent or sending may still send its texels,
 * so they're freed once it's sent, or its lender given back then */
void drop_texture(Held &r, Texture *t)
{
    auto unsent = std::find(r.uploads.begin(), r.uploads.end(), t);
    bool sending = unsent != r.uploads.end() || (*D1_CHCR & CHCR_STR) != 0;

    unplace(r, t);
    if (unsent != r.uploads.end()) {
        r.uploads.erase(unsent);
    }
    r.texture_bytes -= t->gs.Width * t->gs.Height * texel_bits(t->gs.PSM) / 8;
    if (t->lender != nullptr) {
        if (sending) {
            r.returns.push_back(t->lender);
        } else {
            ps2_texture_returned(t->lender);
        }
    } else if (sending) {
        r.dying.push_back(t->gs.Mem);
    } else {
        free(t->gs.Mem);
    }
    delete t;
}

void free_texture(Held &r, uint32_t texture)
{
    if (texture == 0 || texture > r.textures.size() || r.textures[texture - 1] == nullptr) {
        return;
    }
    Texture *t = r.textures[texture - 1];

    /* a fixed texture stays, its place and texels with it, for the next of
     * the game's loads of it (textures.cpp keeps its number) */
    if (t->fixed) {
        return;
    }
    for (Texture *m : t->mirrored) {
        if (m != nullptr) {
            drop_texture(r, m);
        }
    }
    drop_texture(r, t);
    r.textures[texture - 1] = nullptr;
    /* the last texture sent may be this one's room again */
    r.sent = false;
    r.bound = nullptr;
}

} // namespace

uint32_t psp_texture_add(const Image &image)
{
    return add_image(held, image);
}

/* engine/ps2/textures.cpp's: the pack's texels, which the texture keeps */
uint32_t ps2_texture_add_texels(const ps2::TexelsHead &head, void *texels)
{
    return add_texels(held, head, texels);
}

/* and a TIM2's picture, whose texels its lender keeps */
uint32_t ps2_texture_add_picture(const ps2::Lent &lent)
{
    return add_picture(held, lent);
}

/* the PS2 reads no PVR's texels as they lie: each becomes an image first */
uint32_t psp_texture_add_pvr(const std::vector<uint8_t> &, bool &tried)
{
    tried = false;
    return 0;
}

/* the picture's end: the scene's batches after what the walk drew, every
 * triangle in the world that blends sorted together after the opaque, the
 * screen's own pixels last in the game's order, and the picture shows at
 * the display's next blank */
bool psp_submit(const PictureUniform &picture, uint32_t clear, const std::vector<Vertex> &vertices,
                const std::vector<Batch> &batches, bool sorted, const std::vector<float> *places)
{
    Held &r = held;
    const float *v = picture.view;
    Screen s = {picture.project[0], picture.project[1], picture.project[2], picture.project[3]};
    bool blends_drawn = false;
    std::vector<Corner> &in = r.cut_in;

    if (!r.opened) {
        return false;
    }
    if (!r.picture) {
        r.clear = clear;
        open_picture(r);
    }
    /* the next picture opens on this one's color */
    r.clear = clear;
    r.sent = false;
    /* the walk's kept strips join the sort */
    draw_later(r);
    for (const Batch &batch : batches) {
        const State &st = batch.state;
        bool screen = (st.flags & BATCH_SCREEN) != 0;
        Draw d;

        if (screen && !blends_drawn) {
            draw_blends(r);
            blends_drawn = true;
        }
        Texture *t = nullptr;

        if ((st.flags & BATCH_TEXTURE) != 0 && st.texture != 0 && st.texture <= r.textures.size()) {
            t = r.textures[st.texture - 1];
        }
        d.texture = mirrored(r, t, ((st.sample & SAMPLE_FLIP_U) ? 1 : 0) | ((st.sample & SAMPLE_FLIP_V) ? 2 : 0));
        d.decal = (st.flags & BATCH_DECAL) != 0;
        d.alpha = (st.flags & BATCH_OPAQUE) == 0;
        d.linear = (st.sample & SAMPLE_FILTER) != SA_FILTER_POINT;
        /* what the screen's own pixels show is a picture of tiles: each
         * clamps, so its filter takes no texel from the tile's far edge */
        d.clamp_u = (st.sample & SAMPLE_CLAMP_U) != 0 || screen;
        d.clamp_v = (st.sample & SAMPLE_CLAMP_V) != 0 || screen;
        d.blend = st.blend != 0;
        d.src = st.src;
        d.dst = st.dst;
        d.depth = st.depth;
        d.depth_write = st.depth_write != 0 && !(sorted && st.blend != 0);
        d.cull = st.cull != 0 && !screen ? 1 : 0;
        d.offset = (st.flags & BATCH_OFFSET) != 0;
        d.fog = static_cast<uint8_t>(st.flags >> BATCH_FOG_SHIFT & 3);
        const float *m = nullptr;

        if (!screen && places != nullptr && st.place != 0 && st.place * 12U <= places->size()) {
            m = &(*places)[(st.place - 1U) * 12U];
        }
        float su = d.texture != nullptr ? d.texture->su : 0.0F;
        float sv = d.texture != nullptr ? d.texture->sv : 0.0F;

        in.clear();
        for (uint32_t i = batch.first; i < batch.first + batch.count && i < vertices.size(); i++) {
            const Vertex &f = vertices[i];
            Corner c;
            float x = f.at[0], y = f.at[1], z = f.at[2];

            if (screen) {
                c.x = f.at[0];
                c.y = f.at[1] - crop();
                c.z = f.at[2];
            } else {
                if (m != nullptr) {
                    float px = m[0] * x + m[1] * y + m[2] * z + m[3];
                    float py = m[4] * x + m[5] * y + m[6] * z + m[7];
                    float pz = m[8] * x + m[9] * y + m[10] * z + m[11];

                    x = px;
                    y = py;
                    z = pz;
                }
                c.x = v[0] * x + v[1] * y + v[2] * z + v[3];
                c.y = v[4] * x + v[5] * y + v[6] * z + v[7];
                c.z = v[8] * x + v[9] * y + v[10] * z + v[11];
            }
            c.u = f.uv[0] * su;
            c.v = f.uv[1] * sv;
            color_of(f.base, f.offset, d, c);
            in.push_back(c);
        }
        send_triangles(r, d, s, in.data(), static_cast<uint32_t>(in.size()), screen);
    }
    if (!blends_drawn) {
        draw_blends(r);
    }
    {
        /* the GS's FINISH, once it drew all before */
        uint64_t value = 0, name = GS_FINISH;

        send_registers(r, &value, &name, 1);
    }
    uint32_t made = sceKernelGetSystemTimeLow();

    /* the last picture's flip first: its buffer is the one this picture's
     * commands draw into while it shows, until the blank */
    send_stream(r, true);
    flip_pending = static_cast<int32_t>(r.draw_buffer);
    r.draw_buffer ^= 1;
    r.bound = nullptr;
    uint32_t shown = sceKernelGetSystemTimeLow();

    /* the walk's time lies inside the making: direct.cpp says it apart */
    /* a picture that takes a third of a second says what it took */
    if (shown - r.made_since > 300000U) {
        fprintf(stderr,
                "sa1: a slow picture, %u: %u us on the EE and %u on the GS, %u triangles, the strips' %u us, "
                "binds %u us that sent %u textures, the walk's textures %u us; %u bytes of the heap in use, "
                "%u of them textures, and the corners' room %u\n",
                static_cast<unsigned>(r.pictures), static_cast<unsigned>(made - r.made_since),
                static_cast<unsigned>(shown - made), static_cast<unsigned>(r.triangles - r.picture_triangles),
                static_cast<unsigned>(r.strip_us - r.picture_strip_us),
                static_cast<unsigned>(r.bind_us - r.picture_bind_us), static_cast<unsigned>(r.sends - r.picture_sends),
                static_cast<unsigned>(ps2_walk_load_us - r.picture_load_us), static_cast<unsigned>(mallinfo().uordblks),
                static_cast<unsigned>(r.texture_bytes),
                static_cast<unsigned>(r.sent_corners.capacity() * sizeof(Sent) + r.cut_in.capacity() * sizeof(Corner) +
                                      r.blends.capacity() * sizeof(Blend) + r.blend_corners.capacity() * sizeof(Sent)));
    }
    r.make_us += made - r.made_since;
    r.gs_us += shown - made;
    psp_wait_us += shown - made;
    if (++r.timed == 60 || shown - r.reported > 10000000U) {
        r.reported = shown;
        fprintf(stderr,
                "sa1: the GS's last %u pictures: %u triangles, %u us on the EE, %u of them the raster's "
                "strips, %u its binds, which sent %u textures, %u the walk's textures and %u its points' "
                "light, and %u us on the GS each; %u of VU1's batches, %u triangles, %u strips dropped whole, "
                "%u palettes sent; %u bytes of the heap in use, %u of them textures; %u pictures taken back\n",
                static_cast<unsigned>(r.timed), static_cast<unsigned>(r.triangles / r.timed),
                static_cast<unsigned>(r.make_us / r.timed), static_cast<unsigned>(r.strip_us / r.timed),
                static_cast<unsigned>(r.bind_us / r.timed), static_cast<unsigned>(r.sends / r.timed),
                static_cast<unsigned>(ps2_walk_load_us / r.timed), static_cast<unsigned>(ps2_walk_light_us / r.timed),
                static_cast<unsigned>(r.gs_us / r.timed), static_cast<unsigned>(r.vu_batches / r.timed),
                static_cast<unsigned>(r.vu_triangles / r.timed), static_cast<unsigned>(r.strips_past / r.timed),
                static_cast<unsigned>(r.palettes_sent / r.timed), static_cast<unsigned>(mallinfo().uordblks),
                static_cast<unsigned>(r.texture_bytes), static_cast<unsigned>(r.taken_back));
        r.taken_back = 0;
        ps2_walk_load_us = ps2_walk_light_us = 0;
        r.bind_us = 0;
        r.sends = 0;
        r.vu_batches = r.vu_triangles = r.strips_past = r.palettes_sent = 0;
        r.timed = r.triangles = 0;
        r.make_us = r.gs_us = r.strip_us = 0;
    }
    r.picture = false;
    r.pictures++;
    return true;
}

struct Raster::Impl {};

Raster::~Raster() = default;

bool Raster::open(const RasterOptions &, std::string &why)
{
    if (!held.opened) {
        open_screen(held);
    }
    if (!held.opened) {
        why = "no memory for the GS's kept strips";
    }
    return held.opened;
}

std::string Raster::device_name() const
{
    return "the PS2's GS";
}

uint32_t Raster::add_texture(const Image &image, std::string &why)
{
    uint32_t made = add_image(held, image);

    if (made == 0) {
        why = "the GS's raster has no memory for the texture";
    }
    return made;
}

uint32_t Raster::add_pvr(const void *, uint32_t, bool &tried, std::string &)
{
    tried = false;
    return 0;
}

void Raster::free_texture(uint32_t texture)
{
    pc::free_texture(held, texture);
}

float Raster::reach() const
{
    return REACH;
}

bool Raster::submit(uint32_t, const PictureUniform &picture, uint32_t clear, const std::vector<Vertex> &vertices,
                    const std::vector<Batch> &batches, bool sorted, std::string &why,
                    const std::vector<float> *places)
{
    if (!psp_submit(picture, clear, vertices, batches, sorted, places)) {
        why = "the GS's raster isn't open";
        return false;
    }
    return true;
}

void Raster::wide(bool)
{
}

bool Raster::broken(std::string &) const
{
    return false;
}

bool Raster::done(uint32_t)
{
    return true;
}

void Raster::wait(uint32_t)
{
}

uint32_t Raster::lost(uint32_t)
{
    return 0;
}

uint32_t Raster::cut(uint32_t)
{
    return 0;
}

uint64_t Raster::gpu_ns(uint32_t)
{
    return 0;
}

bool Raster::read(uint32_t, void *)
{
    return false;
}

} // namespace pc

using pc::held;

extern "C" int psp_picture_open(void)
{
    if (!held.opened) {
        return 0;
    }
    pc::open_picture(held);
    return 1;
}

extern "C" void psp_draw_screen(float focal_x, float focal_y, float center_x, float center_y)
{
    const float p[4] = {focal_x, focal_y, center_x, center_y};

    if (memcmp(p, held.place.screen, sizeof p) != 0) {
        memcpy(held.place.screen, p, sizeof p);
        held.place_kept = false;
    }
}

extern "C" void psp_draw_place(const float *matrix)
{
    memcpy(held.place.matrix, matrix, sizeof held.place.matrix);
    held.place_kept = false;
}

extern "C" void psp_draw_state(const struct psp_state *state)
{
    held.state = *state;
}

extern "C" struct psp_corner *psp_draw_room(uint32_t corners, int later)
{
    pc::Held &r = held;

    if (!r.picture || corners == 0 || corners > 65535) {
        return nullptr;
    }
    if (later != 0) {
        if (r.kept_used + corners > pc::KEPT_CORNERS) {
            return nullptr;
        }
        psp_corner *at = r.kept + r.kept_used;

        r.kept_used += corners;
        return at;
    }
    if (r.room.size() < corners) {
        r.room.resize(corners);
    }
    return r.room.data();
}

extern "C" void psp_draw_strip(const struct psp_corner *corners, uint32_t count)
{
    if (held.picture) {
        pc::send_corners(held, held.place, held.state, corners, count, false);
    }
}

extern "C" void psp_draw_later(float depth, const struct psp_state *state, const struct psp_corner *corners,
                               uint32_t count)
{
    pc::Held &r = held;

    if (!r.picture || corners < r.kept || corners >= r.kept + pc::KEPT_CORNERS) {
        return;
    }
    pc::keep_place(r);
    r.later.push_back(
        {depth, static_cast<uint32_t>(r.places.size() - 1), *state, static_cast<uint32_t>(corners - r.kept), count, false});
}

/* a strip that may reach past the frame: the GS's raster cuts every
 * triangle it sends, so the strip goes as any other, as triangles in the
 * strip's turn. a later one is kept, its corners copied */
extern "C" void psp_draw_cut(const struct psp_corner *corners, uint32_t count, int reversed,
                             const struct psp_state *later, float depth)
{
    pc::Held &r = held;
    std::vector<psp_corner> &tri = r.cut_strip;

    tri.clear();

    if (!r.picture || count < 3) {
        return;
    }
    /* most such strips lie within every side all the same */
    if (later == nullptr) {
        uint32_t began = stamp();
        bool drawn = pc::vu_strip(r, r.place, pc::draw_of(r, r.state), corners, count, reversed != 0);

        r.strip_us += stamp() - began;
        if (drawn) {
            return;
        }
    }
    for (uint32_t i = 0; i + 2 < count; i++) {
        bool flip = ((i & 1) != 0) != (reversed != 0);

        tri.push_back(corners[flip ? i + 1 : i]);
        tri.push_back(corners[flip ? i : i + 1]);
        tri.push_back(corners[i + 2]);
    }
    uint32_t made = static_cast<uint32_t>(tri.size());

    if (later == nullptr) {
        pc::send_corners(r, r.place, r.state, tri.data(), made, true);
        return;
    }
    if (r.kept_used + made > pc::KEPT_CORNERS) {
        return;
    }
    memcpy(r.kept + r.kept_used, tri.data(), made * sizeof(psp_corner));
    pc::keep_place(r);
    r.later.push_back({depth, static_cast<uint32_t>(r.places.size() - 1), *later, r.kept_used, made, true});
    r.kept_used += made;
}

/* a model batch's head by UNPACK to VU1's buffer, then its corners', whose
 * count ps2_vu_close writes: the room for the most corners goes first, so
 * nothing else writes the stream between */
extern "C" void ps2_vu_palette(uint32_t key, const uint32_t *colors, const uint32_t *offsets)
{
    pc::Held &r = held;

    if (!r.picture || r.model_open || r.vu_palette == key) {
        return;
    }
    /* VU1's batches before read the palette there: it goes after them */
    pc::room(r, 8 + pc::VU_PALETTE_ENTRIES * 4 + 8);
    pc::close_direct(r);
    pc::put(r, pc::vif_code(pc::VIF_FLUSHE, 0, 0));
    while (r.at % 4 != 3) {
        pc::put(r, 0);
    }
    pc::put(r, pc::vif_code(pc::UNPACK_V4_32, pc::VU_PALETTE_ENTRIES & 0xff, pc::VU_PALETTE));
    float *f = reinterpret_cast<float *>(pc::words(r) + r.at);

    for (uint32_t e = 0; e < pc::VU_PALETTE_ENTRIES; e++) {
        uint32_t color = colors[e * 2];

        /* ARGB's low 24 bits are red times 65536 plus green times 256 plus
         * blue, a whole number a float holds exactly */
        f[4 * e] = static_cast<float>(color & 0xffffffU);
        f[4 * e + 1] = offsets != nullptr ? static_cast<float>(offsets[e * 2] & 0xffffffU) : 0.0F;
        f[4 * e + 2] = static_cast<float>(color >> 24);
        f[4 * e + 3] = 0.0F;
    }
    r.at += pc::VU_PALETTE_ENTRIES * 4;
    r.vu_palette = key;
    r.palettes_sent++;
}

extern "C" float *ps2_vu_open(const struct psp_state *state, const struct ps2_vu_shade *shade)
{
    pc::Held &r = held;

    if (!r.picture || r.model_open) {
        return nullptr;
    }
    pc::Draw d = pc::draw_of(r, *state);

    pc::apply(r, d);
    pc::room(r, 8 + pc::VU_MODEL_HEAD * 4 + 4 + PS2_VU_CORNERS * 8 + 8);
    pc::close_direct(r);
    while (r.at % 4 != 3) {
        pc::put(r, 0);
    }
    pc::put(r, pc::vif_code(pc::UNPACK_V4_32, pc::VU_MODEL_HEAD, pc::UNPACK_TOPS));
    uint32_t *w = pc::words(r) + r.at;
    float *h = reinterpret_cast<float *>(w);

    r.model_prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRISTRIP, 1, 0, 0, d.blend ? 1 : 0, 0, 0, 0, 0);
    memset(w, 0, pc::VU_MODEL_HEAD * 16);
    memcpy(h + 8, r.place.matrix, 16 * sizeof(float));
    h[24] = r.place.screen[0];
    h[25] = r.place.screen[1];
    h[26] = pc::DEPTH_MOST;
    h[28] = r.place.screen[2] + static_cast<float>(r.gs->OffsetX) / 16.0F;
    h[29] = r.place.screen[3] - pc::crop() + static_cast<float>(r.gs->OffsetY) / 16.0F;
    /* the GS's 1 is 0x80: alpha always halved, the three where a texture
     * modulates them */
    float halve = d.texture != nullptr && !d.decal ? 128.0F / 255.0F : 1.0F;

    h[32] = h[33] = h[34] = halve;
    h[35] = 128.0F / 255.0F;
    h[36] = 1e30F;
    h[39] = 2048.0F;
    memcpy(h + 40, shade->light, 3 * sizeof(float));
    memcpy(h + 44, shade->scale, 4 * sizeof(float));
    memcpy(h + 48, shade->add, 4 * sizeof(float));
    h[52] = 127.5F;
    h[53] = 255.0F;
    h[54] = 1.0F / 65536.0F;
    h[55] = 1.0F / 256.0F;
    h[56] = 65536.0F;
    h[57] = 256.0F;
    r.model_head = r.at;
    r.at += pc::VU_MODEL_HEAD * 4;
    r.model_unpack = r.at;
    pc::put(r, 0);
    r.model_open = true;
    return reinterpret_cast<float *>(pc::words(r) + r.at);
}

extern "C" void ps2_vu_close(uint32_t count)
{
    pc::Held &r = held;

    if (!r.model_open) {
        return;
    }
    r.model_open = false;
    if (count < 3 || count > PS2_VU_CORNERS) {
        /* the head's UNPACK goes too: the NOPs before it are VIF1's own */
        r.at = r.model_head - 1;
        return;
    }
    uint32_t *w = pc::words(r);
    u64 *tag = reinterpret_cast<u64 *>(w + r.model_head);

    tag[0] = GIF_TAG(count, 1, 1, r.model_prim, GSKIT_GIF_FLG_PACKED, 2);
    tag[1] = GS_RGBAQ | pc::GS_XYZF2_REG << 4;
    w[r.model_head + 4] = count;
    w[r.model_unpack] =
        pc::vif_code(pc::UNPACK_V4_32, (count * 2) & 0xff, pc::UNPACK_TOPS | pc::VU_MODEL_HEAD);
    r.at = r.model_unpack + 1 + count * 8;
    pc::put(r, pc::vif_code(pc::VIF_MSCAL, 0, pc::VU_MODEL_AT));
    r.vu_since = true;
    r.triangles += count - 2;
    r.vu_triangles += count - 2;
    r.vu_batches++;
}

extern "C" void psp_picture_drop(void)
{
    pc::Held &r = held;

    if (r.picture) {
        pc::take_back(r);
    }
    r.picture = false;
    r.later.clear();
    r.blends.clear();
    r.blend_corners.clear();
    r.kept_used = 0;
}

/* the GS's raster keeps nothing in the loader's work area */
extern "C" void psp_draw_yield(void)
{
}

extern "C" float psp_draw_reach(void)
{
    return pc::REACH;
}

extern "C" void psp_draw_report(void)
{
    pc::Held &r = held;

    fprintf(stderr,
            "sa1: the GS drew %u pictures; %u textures at most, %u mirrored, %u bytes of them now, a "
            "picture's commands %u bytes at most, %u triangles lost, %u blends sorted at most and %u "
            "past the sort's room\n",
            static_cast<unsigned>(r.pictures), static_cast<unsigned>(r.most_textures),
            static_cast<unsigned>(r.mirrors), static_cast<unsigned>(r.texture_bytes),
            static_cast<unsigned>(r.most_queue), static_cast<unsigned>(r.lost), static_cast<unsigned>(r.most_blends),
            static_cast<unsigned>(r.blends_past));
}

extern "C" void psp_texture_free(uint32_t texture)
{
    pc::free_texture(held, texture);
}

/* texels the game made in memory, as a PVR's chunk holds them: they become
 * a chunk, which the PC's reader decodes */
extern "C" uint32_t psp_texture_texels(uint32_t pixel, uint32_t layout, uint32_t width, uint32_t height,
                                       const void *texels, uint32_t bytes)
{
    std::vector<uint8_t> chunk(16 + bytes);
    pc::Image image;
    std::string why;
    uint32_t length = 8 + bytes;

    memcpy(chunk.data(), "PVRT", 4);
    memcpy(chunk.data() + 4, &length, 4);
    chunk[8] = static_cast<uint8_t>(pixel);
    chunk[9] = static_cast<uint8_t>(layout);
    chunk[12] = static_cast<uint8_t>(width);
    chunk[13] = static_cast<uint8_t>(width >> 8);
    chunk[14] = static_cast<uint8_t>(height);
    chunk[15] = static_cast<uint8_t>(height >> 8);
    memcpy(chunk.data() + 16, texels, bytes);
    if (!pc::pvr_decode(chunk, 0, image, why)) {
        return 0;
    }
    return pc::add_image(held, image);
}

extern "C" int psp_texture_cuts(uint32_t texture)
{
    return texture != 0 && texture <= held.textures.size() && held.textures[texture - 1] != nullptr &&
           held.textures[texture - 1]->cuts;
}
