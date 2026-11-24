/* the PS2's own model path, which records.c includes beside the walk
 * (engine/psp/sa1/walk.h) and draws by in its place where the settings'
 * models 1 asks (engine/ps2/main.c). the walk lights and makes each corner
 * on the EE; here the EE only gathers each corner's point and normal into a
 * batch of VU1's (engine/ps2/model.vsm, through gs.cpp's ps2_vu_open), and
 * VU1 does the rest. it comes in stages, each drawn and checked before the
 * next:
 *
 * 1. a model's polygons, every one opaque and untextured. a strip of a
 *    model whose ball reaches past the raster's reach goes whole where its
 *    box lies within every side, not at all where it lies past one side,
 *    and else by the walk's cut, on the EE
 * 2. SA1's palettes' light, as the walk gives it: a point takes the entry
 *    of the picture's palette its normal's dot with the light picks, and a
 *    meshset's material whitens, holds a constant color or takes its own
 *    alpha as the walk's does. the palette goes to VU1's memory, and the
 *    material's ways go as a scale of the entry's color and what adds to
 *    it. a model under Ninja's own drawers takes a flat color of its
 *    meshset's still
 *
 * a corner's word (model.vsm): 2 where it starts a strip, so it draws
 * nothing, else the cull's turn for the triangle it ends */
#define MODELS_START 2.0F

/* the color of a model's meshset, by both: each of the three from 64 to
 * 255, so each meshset stands out from the next, ARGB */
static uint32_t models_color(const NJS_MODEL *model, uint32_t set)
{
    uint32_t h = (uint32_t)(uintptr_t)model * 2654435761U + (set + 1) * 40503U;

    h ^= h >> 13;
    h *= 2246822519U;
    h ^= h >> 16;
    return 0xff000000U | (64U + (h & 0xbfU)) << 16 | (64U + (h >> 8 & 0xbfU)) << 8 | (64U + (h >> 16 & 0xbfU));
}

/* shade adds argb whole and takes none of the palette's */
static void models_flat(struct ps2_vu_shade *shade, uint32_t argb)
{
    for (int k = 0; k < 4; k++) {
        shade->scale[k] = 0.0F;
    }
    shade->add[0] = (float)(argb >> 16 & 0xff);
    shade->add[1] = (float)(argb >> 8 & 0xff);
    shade->add[2] = (float)(argb & 0xff);
    shade->add[3] = (float)(argb >> 24);
}

/* a corner's color, RGBA in memory, as model.vsm makes it of the palette's
 * colors, for a strip the EE cuts */
static uint32_t models_lit(const struct ps2_vu_shade *shade, const uint32_t *colors,
                           const NJS_VECTOR *normal)
{
    float dot = normal ? normal->x * shade->light[0] + normal->y * shade->light[1] +
                             normal->z * shade->light[2]
                       : 0.0F;
    float scaled = dot * 127.5F + 127.5F;
    int entry = scaled > 0.0F ? (scaled < 255.0F ? (int)scaled : 255) : 0;
    uint32_t word = colors ? colors[entry * 2] : 0;
    float from[4] = {(float)(word >> 16 & 0xff), (float)(word >> 8 & 0xff), (float)(word & 0xff),
                     (float)(word >> 24)};
    uint32_t rgba = 0;

    for (int k = 0; k < 4; k++) {
        float v = from[k] * shade->scale[k] + shade->add[k];
        uint32_t byte = v <= 0.0F ? 0U : v >= 255.0F ? 255U : (uint32_t)v;

        rgba |= byte << (8 * k);
    }
    return rgba;
}

/* where a strip's box lies against the raster's sides, as gs.cpp's box_of
 * finds it: 0 within every side, 1 past one side whole, 2 across */
static int models_box(const float *m, const NJS_POINT3 *points, uint32_t count_points,
                      const Sint16 *at, uint32_t n)
{
    float lo[3], hi[3];
    float reach = psp_draw_reach();
    float fx = sa1_screen.scale_x, fy = sa1_screen.scale_y;
    float ox = sa1_screen.cx - 320.0F, oy = sa1_screen.cy - 240.0F;
    uint32_t every = 31, any = 0;

    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (uint16_t)at[k];
        const NJS_POINT3 *p = &points[i < count_points ? i : 0];

        if (k == 0) {
            lo[0] = hi[0] = p->x;
            lo[1] = hi[1] = p->y;
            lo[2] = hi[2] = p->z;
            continue;
        }
        lo[0] = p->x < lo[0] ? p->x : lo[0];
        hi[0] = p->x > hi[0] ? p->x : hi[0];
        lo[1] = p->y < lo[1] ? p->y : lo[1];
        hi[1] = p->y > hi[1] ? p->y : hi[1];
        lo[2] = p->z < lo[2] ? p->z : lo[2];
        hi[2] = p->z > hi[2] ? p->z : hi[2];
    }
    for (int c = 0; c < 8; c++) {
        float x = c & 1 ? hi[0] : lo[0], y = c & 2 ? hi[1] : lo[1], z = c & 4 ? hi[2] : lo[2];
        float vx = x * m[0] + y * m[4] + z * m[8] + m[12];
        float vy = x * m[1] + y * m[5] + z * m[9] + m[13];
        float vz = x * m[2] + y * m[6] + z * m[10] + m[14];
        float sx = fx * vx + ox * vz, sy = fy * vy + oy * vz;
        float wide = reach * 320.0F * vz, high = reach * 240.0F * vz;
        uint32_t past = (vz - 1.0F < 0.0F ? 1U : 0U) | (wide - sx < 0.0F ? 2U : 0U) |
                        (wide + sx < 0.0F ? 4U : 0U) | (high - sy < 0.0F ? 8U : 0U) |
                        (high + sy < 0.0F ? 16U : 0U);

        every &= past;
        any |= past;
    }
    return every != 0 ? 1 : any != 0 ? 2 : 0;
}

/* a strip across a side, by the walk's cut on the EE, lit as VU1 lights */
static void models_cut(const NJS_MODEL *model, uint32_t count_points, const Sint16 *at, uint32_t n,
                       int reversed, const struct psp_state *drawn, const struct ps2_vu_shade *shade,
                       const uint32_t *colors)
{
    static struct psp_corner strip[WALK_STRIP];

    if (n > WALK_STRIP) {
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (uint16_t)at[k];
        const NJS_POINT3 *p;

        i = i < count_points ? i : 0;
        p = &model->points[i];
        strip[k] = (struct psp_corner){.color = models_lit(shade, colors, model->normals ? &model->normals[i] : NULL),
                                       .x = p->x,
                                       .y = p->y,
                                       .z = p->z};
    }
    psp_draw_state(drawn);
    psp_draw_cut(strip, n, reversed, NULL, 0.0F);
}

/* a corner's point and normal into a batch, with its word */
static inline void models_corner(float *out, const NJS_MODEL *model, uint32_t count_points, uint32_t i,
                                 float word)
{
    const NJS_POINT3 *p;

    i = i < count_points ? i : 0;
    p = &model->points[i];
    out[0] = p->x;
    out[1] = p->y;
    out[2] = p->z;
    out[3] = word;
    if (model->normals) {
        const NJS_VECTOR *n = &model->normals[i];

        out[4] = n->x;
        out[5] = n->y;
        out[6] = n->z;
    } else {
        out[4] = out[5] = out[6] = 0.0F;
    }
    out[7] = 0.0F;
}

/* draw model under state, 1, or leave it out, 0 */
static int models_draw(const NJS_MODEL *model, uint8_t mode, const struct draw_state *state)
{
    const float *m = native_xmtrx();
    const NJS_POINT3 *points = model->points;
    uint32_t count_points = (uint32_t)model->nbPoint;
    uint32_t count = materials_named(model);
    float longest = 0.0F, far = 0.0F, radius, eye[3];
    float across = psp_draw_reach() * 320.0F / sa1_screen.scale_x;
    float down = psp_draw_reach() * 240.0F / sa1_screen.scale_y;
    const NJS_POINT3 *c = &model->center;
    double seen[3] = {sa1_live_light.x, sa1_live_light.y, sa1_live_light.z};
    int by_palette = mode == SA_SHADE_PALETTE;
    const uint32_t *colors = NULL;
    uint32_t constant;
    float light[3];
    int cutting, white = 0;

    if (!walk_open || model->nbMeshset == 0 || count_points == 0 || count > WALK_MATERIALS) {
        return 0;
    }
    /* the light in the model's space, by its dots with the rows that carry
     * the model's axes to the view's, as the walk finds it */
    for (int k = 0; k < 3; k++) {
        float length = m[4 * k] * m[4 * k] + m[4 * k + 1] * m[4 * k + 1] + m[4 * k + 2] * m[4 * k + 2];

        light[k] = m[4 * k] * (float)seen[0] + m[4 * k + 1] * (float)seen[1] + m[4 * k + 2] * (float)seen[2];
        longest = length > longest ? length : longest;
    }
    /* the model's ball in view space, as the walk finds it: within the
     * raster's reach of the frame on each side, its strips go as they are */
    for (uint32_t i = 0; i < count_points; i++) {
        float dx = points[i].x - c->x, dy = points[i].y - c->y, dz = points[i].z - c->z;
        float apart = dx * dx + dy * dy + dz * dz;

        far = apart > far ? apart : far;
    }
    for (int j = 0; j < 3; j++) {
        eye[j] = c->x * m[j] + c->y * m[4 + j] + c->z * m[8 + j] + m[12 + j];
    }
    radius = sqrtf(far * longest);
    cutting = !(across * eye[2] - fabsf(eye[0]) >= radius * sqrtf(1.0F + across * across)) ||
              !(down * eye[2] - fabsf(eye[1]) >= radius * sqrtf(1.0F + down * down));
    /* the picture's palette, and the offsets of the next where the model's
     * first material asks for a highlight, as the walk reads them */
    if (by_palette && model->normals) {
        uint32_t spec = count > 0 && (material_as_drawn(&model->mats[0], mode, state, 0).flags & SA_MAT_SPECULAR);
        uint32_t pal = (state->palette & 7U) + (spec ? 1U : 0U);

        colors = sa1_live_colors + state->palette * SA_PALETTE_ENTRIES * 2;
        ps2_vu_palette(state->palette | spec << 8, colors,
                       pal < SA_CAP_PALETTES_COUNT ? sa1_live_colors + pal * SA_CAP_PALETTE_ENTRIES * 2 + 1 : NULL);
    }
    constant = argb(1.0, state->material.r, state->material.g, state->material.b);
    view_id(1);
    psp_draw_screen(sa1_screen.scale_x, sa1_screen.scale_y, sa1_screen.cx, sa1_screen.cy);
    psp_draw_place(m);
    for (uint32_t s = 0; s < model->nbMeshset; s++) {
        const NJS_MESHSET *set = &model->meshsets[s];
        uint32_t named = set->type_matId & MATERIAL_ID;
        sa_cap_material material = named < count ? material_as_drawn(&model->mats[named], mode, state, 0)
                                                 : (sa_cap_material){0};
        int cull = (material.flags & SA_MAT_CULL) != 0;
        int blended = (material.flags & SA_MAT_BLEND) != 0;
        struct ps2_vu_shade shade = {.light = {light[0], light[1], light[2]},
                                     .scale = {1.0F, 1.0F, 1.0F, 1.0F}};
        struct psp_state drawn = {
            .depth = (uint8_t)((state->isp & ISP_DEPTH) >> ISP_DEPTH_SHIFT),
            .depth_write = !(state->isp & ISP_Z_WRITE_DISABLE),
        };
        const Sint16 *at = set->meshes;
        float *out = NULL;
        uint32_t used = 0;

        /* SA1's shade, as the walk's: a meshset under SA_MAT_WHITE leaves
         * the draw's points white for itself and each meshset after it */
        white |= (material.flags & SA_MAT_WHITE) != 0;
        if (!by_palette) {
            models_flat(&shade, models_color(model, s));
        } else {
            /* a point's color is its entry's: white where a meshset that
             * blends is unlit, and once a meshset whitened. one that blends
             * takes its material's alpha. a constant color stands in every
             * point's place, and with no normals each point is white */
            int whiten = white || (blended && (material.flags & SA_MAT_UNLIT));
            uint32_t alpha = blended ? (uint32_t)material.alpha : 255U;

            if (material.flags & SA_MAT_CONSTANT) {
                models_flat(&shade, (constant & 0x00ffffffU) | alpha << 24);
            } else if (!model->normals) {
                models_flat(&shade, 0x00ffffffU | alpha << 24);
            } else {
                for (int k = 0; k < 3; k++) {
                    shade.scale[k] = whiten ? 0.0F : 1.0F;
                    shade.add[k] = whiten ? 255.0F : 0.0F;
                }
                shade.scale[3] = blended ? 0.0F : 1.0F;
                shade.add[3] = blended ? (float)alpha : 0.0F;
            }
        }
        for (uint32_t p = 0; p < set->nbMesh; p++) {
            uint32_t n;
            int reversed;

            walk_polygon(set, &at, &n, &reversed);
            if (n < 3) {
                at += n;
                continue;
            }
            if (cutting) {
                int box = models_box(m, points, count_points, at, n);

                if (box == 1) {
                    at += n;
                    continue;
                }
                if (box == 2) {
                    /* the cut writes the stream: the open batch goes first */
                    if (out) {
                        ps2_vu_close(used);
                        out = NULL;
                    }
                    drawn.cull = (uint8_t)cull;
                    models_cut(model, count_points, at, n, reversed, &drawn, &shade, colors);
                    at += n;
                    continue;
                }
            }
            /* the strip's corners into the batch, a new batch where it's
             * full, which goes on from the strip's last two corners */
            drawn.cull = 0;
            for (uint32_t k = 0; k < n; k++) {
                if (!out || used == PS2_VU_CORNERS) {
                    uint32_t again = k >= 2 ? 2 : k;

                    if (out) {
                        ps2_vu_close(used);
                    }
                    if (!(out = ps2_vu_open(&drawn, &shade))) {
                        return 1;
                    }
                    used = 0;
                    for (uint32_t j = k - again; j < k; j++) {
                        models_corner(out + 8 * used++, model, count_points, (uint16_t)at[j], MODELS_START);
                    }
                }
                /* the triangle a corner ends turns as the strip's first, or
                 * the other way for each odd one, and for a reversed strip */
                models_corner(out + 8 * used++, model, count_points, (uint16_t)at[k],
                              k < 2   ? MODELS_START
                              : !cull ? 0.0F
                                      : ((k - 2) & 1) != (uint32_t)(reversed != 0) ? -1.0F
                                                                                   : 1.0F);
            }
            at += n;
        }
        if (out) {
            ps2_vu_close(used);
        }
    }
    return 1;
}
