/* the PS2's own model path, which records.c includes beside the walk
 * (engine/psp/sa1/walk.h) and draws by in its place where the settings'
 * models 1 asks (engine/ps2/main.c). the walk lights and makes each corner
 * on the EE; here the EE only gathers each corner's point, normal and u and
 * v into a batch of VU1's (engine/ps2/model.vsm, through gs.cpp's
 * ps2_vu_open), and VU1 does the rest. it came in stages, each drawn and
 * checked before the next (engine/ps2/RENDERER.md):
 *
 * 1. a model's polygons. a strip of a model whose ball reaches past the
 *    raster's reach goes whole where its box lies within every side, not at
 *    all where it lies past one side, and else by the walk's cut, on the EE
 * 2. SA1's palettes' light, as the walk gives it: a point takes the entry
 *    of the picture's palette its normal's dot with the light picks, and a
 *    meshset's material whitens, holds a constant color or takes its own
 *    alpha as the walk's does. the palette goes to VU1's memory, and the
 *    material's ways go as a scale of the entry's color and what adds to it
 * 3. textures, the fog, and highlights: a corner's u and v are the
 *    meshset's, or an env map's of its normal; a highlight is the palette's
 *    offset, or the unlit words' where the walk takes those. a meshset that
 *    blends goes as the walk's do, on the EE, to the picture's sort of the
 *    blends, and a model under Ninja's own drawers goes by the walk
 *
 * a corner's word (model.vsm): 2 where it starts a strip, so it draws
 * nothing, else the cull's turn for the triangle it ends */
#define MODELS_START 2.0F

/* what the path did, for the raster's report (gs.cpp): the models drawn,
 * the points their balls took, the strips whose boxes were found, the
 * corners gathered for VU1, and those lit on the EE */
uint32_t ps2_models_drawn, ps2_models_points, ps2_models_boxes, ps2_models_corners, ps2_models_ee;

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

/* the palette's entry a normal's dot with the light picks */
static uint32_t models_entry(const struct ps2_vu_shade *shade, const NJS_VECTOR *normal)
{
    float dot = normal ? normal->x * shade->light[0] + normal->y * shade->light[1] + normal->z * shade->light[2]
                       : 0.0F;
    float scaled = dot * 127.5F + 127.5F;

    return scaled > 0.0F ? (scaled < 255.0F ? (uint32_t)scaled : 255U) : 0U;
}

/* a corner's color, RGBA in memory, as model.vsm makes it of the palette's
 * colors, for a strip the EE draws */
static uint32_t models_lit(const struct ps2_vu_shade *shade, const uint32_t *colors, uint32_t entry)
{
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

/* the most of an ARGB word's three */
static uint32_t models_most(uint32_t argb)
{
    uint32_t r = argb >> 16 & 0xff, g = argb >> 8 & 0xff, b = argb & 0xff;
    uint32_t m = r > g ? r : g;

    return m > b ? m : b;
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

/* what a meshset's corners are made of, on VU1 or the EE */
struct models_set {
    const NJS_MODEL *model;
    const float *m;
    uint32_t count_points;
    const NJS_COLOR *uv;           /* the meshset's, by corner, or null */
    int env;                       /* its u and v are the normal's, in view space */
    const uint32_t *colors;        /* the palette's, or null */
    const uint32_t *offsets;       /* its offsets' palette, or null */
    int unlit_offsets;             /* the offset is the unlit words', by point */
    struct ps2_vu_shade shade;
};

/* point i's u and v at corner, 255 to the texture's side */
static inline void models_uv(const struct models_set *w, uint32_t i, uint32_t corner, float *u, float *v)
{
    if (w->env) {
        const NJS_VECTOR *n = &w->model->normals[i];
        const float *m = w->m;

        *u = ((n->x * m[0] + n->y * m[4] + n->z * m[8]) * 0.5F + 0.5F) * 255.0F;
        *v = ((n->x * m[1] + n->y * m[5] + n->z * m[9]) * 0.5F + 0.5F) * 255.0F;
    } else if (w->uv) {
        *u = (float)w->uv[corner].tex.u;
        *v = (float)w->uv[corner].tex.v;
    } else {
        *u = *v = 0.0F;
    }
}

/* point i's offset, ARGB */
static inline uint32_t models_offset(const struct models_set *w, uint32_t i, uint32_t entry)
{
    if (w->unlit_offsets) {
        return sa1_live_colors[SA_CAP_PALETTES_COUNT * SA_CAP_PALETTE_ENTRIES * 2 +
                               (i < SA_CAP_UNLIT_OFFSETS ? i : 0)];
    }
    return w->offsets ? w->offsets[entry * 2] : 0;
}

/* a corner of point i, the meshset's corner'th, into a batch, with its word */
static inline void models_corner(float *out, const struct models_set *w, uint32_t i, uint32_t corner, float word)
{
    const NJS_POINT3 *p;

    i = i < w->count_points ? i : 0;
    p = &w->model->points[i];
    out[0] = p->x;
    out[1] = p->y;
    out[2] = p->z;
    out[3] = word;
    if (w->model->normals) {
        const NJS_VECTOR *n = &w->model->normals[i];

        out[4] = n->x;
        out[5] = n->y;
        out[6] = n->z;
    } else {
        out[4] = out[5] = out[6] = 0.0F;
    }
    out[7] = w->shade.corner_offsets ? (float)models_most(models_offset(w, i, 0)) : 0.0F;
    models_uv(w, i, corner, &out[8], &out[9]);
    out[10] = out[11] = 0.0F;
}

/* a strip's corners as the raster takes them, lit as VU1 lights, for the
 * EE's draws: a strip across a side, and one that blends */
static void models_strip(struct psp_corner *strip, const struct models_set *w, const Sint16 *at, uint32_t n,
                         uint32_t corner)
{
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (uint16_t)at[k];
        const NJS_VECTOR *normal;
        uint32_t entry;
        float u, v;

        i = i < w->count_points ? i : 0;
        normal = w->model->normals ? &w->model->normals[i] : NULL;
        entry = models_entry(&w->shade, normal);
        models_uv(w, i, corner + k, &u, &v);
        strip[k] = (struct psp_corner){.u = (int16_t)(u > 32767.0F ? 32767 : u < -32768.0F ? -32768 : (int)u),
                                       .v = (int16_t)(v > 32767.0F ? 32767 : v < -32768.0F ? -32768 : (int)v),
                                       .color = models_lit(&w->shade, w->colors, entry),
                                       .x = w->model->points[i].x,
                                       .y = w->model->points[i].y,
                                       .z = w->model->points[i].z,
                                       .offset = walk_rgba(models_offset(w, i, entry))};
    }
}

/* the sides of the raster's reach a point lies past, as models_box tests
 * them: bit 0 nearer than the near plane, then the four sides */
static inline uint32_t models_sides(const float *m, const NJS_POINT3 *p, float reach)
{
    float vx = p->x * m[0] + p->y * m[4] + p->z * m[8] + m[12];
    float vy = p->x * m[1] + p->y * m[5] + p->z * m[9] + m[13];
    float vz = p->x * m[2] + p->y * m[6] + p->z * m[10] + m[14];
    float sx = sa1_screen.scale_x * vx + (sa1_screen.cx - 320.0F) * vz;
    float sy = sa1_screen.scale_y * vy + (sa1_screen.cy - 240.0F) * vz;
    float wide = reach * 320.0F * vz, high = reach * 240.0F * vz;

    return (vz - 1.0F < 0.0F ? 1U : 0U) | (wide - sx < 0.0F ? 2U : 0U) | (wide + sx < 0.0F ? 4U : 0U) |
           (high - sy < 0.0F ? 8U : 0U) | (high + sy < 0.0F ? 16U : 0U);
}

/* the depth of a strip's middle corner, which a kept strip sorts by */
static inline float models_depth(const float *m, const NJS_POINT3 *points, uint32_t count_points, const Sint16 *at,
                                 uint32_t n)
{
    uint32_t i = (uint16_t)at[n / 2];
    const NJS_POINT3 *p = &points[i < count_points ? i : 0];

    return p->x * m[2] + p->y * m[6] + p->z * m[10] + m[14];
}

/* the open batch of VU1's, and its corners */
struct models_batch {
    float *out;
    uint32_t used;
};

/* room in the open batch for count corners more, or a new batch: where a
 * run of the strip's goes on, the new one starts again from its last two
 * corners, k's two before. 0 where no picture is open */
static int models_room(struct models_batch *b, uint32_t count, int goes_on, const struct psp_state *drawn,
                       const struct models_set *w, const Sint16 *at, uint32_t k, uint32_t corner)
{
    if (b->out && b->used + count <= PS2_VU_CORNERS) {
        return 1;
    }
    if (b->out) {
        ps2_vu_close(b->used);
    }
    if (!(b->out = ps2_vu_open(drawn, &w->shade))) {
        return 0;
    }
    b->used = 0;
    for (uint32_t j = goes_on ? k - 2 : k; j < k; j++) {
        models_corner(b->out + 12 * b->used++, w, (uint16_t)at[j], corner + j, MODELS_START);
    }
    return 1;
}

/* the open batch goes, as the EE's draw writes the stream */
static void models_close(struct models_batch *b)
{
    if (b->out) {
        ps2_vu_close(b->used);
        b->out = NULL;
    }
}

/* corner k's word: the triangle it ends turns as the strip's first, or the
 * other way for each odd one, and for a reversed strip */
static inline float models_word(uint32_t k, int cull, int reversed)
{
    return k < 2 ? MODELS_START : !cull ? 0.0F : ((k - 2) & 1) != (uint32_t)(reversed != 0) ? -1.0F : 1.0F;
}

/* draw model under state, 1, or leave it out, 0 */
static int models_draw(const NJS_MODEL *model, uint8_t mode, const struct draw_state *state)
{
    static sa_cap_material materials[WALK_MATERIALS];
    static uint8_t cuts[WALK_MATERIALS];
    static struct psp_corner strip[WALK_STRIP];
    static uint8_t sides[WALK_STRIP];
    const float *m = native_xmtrx();
    float reach = psp_draw_reach();
    const NJS_POINT3 *points = model->points;
    uint32_t count_points = (uint32_t)model->nbPoint;
    uint32_t count = materials_named(model);
    float longest = 0.0F, far = 0.0F, radius, eye[3];
    float across = psp_draw_reach() * 320.0F / sa1_screen.scale_x;
    float down = psp_draw_reach() * 240.0F / sa1_screen.scale_y;
    const NJS_POINT3 *c = &model->center;
    double seen[3] = {sa1_live_light.x, sa1_live_light.y, sa1_live_light.z};
    const uint32_t *colors = NULL, *offsets = NULL;
    uint32_t constant;
    float light[3];
    int cutting, white = 0;

    /* Ninja's own drawers light by their own ways, which the walk has */
    if (mode != SA_SHADE_PALETTE) {
        return walk_model(model, mode, state);
    }
    if (!walk_open || model->nbMeshset == 0 || count_points == 0 || count > WALK_MATERIALS) {
        return 0;
    }
    /* the materials as the walk takes them, its textures made at the first
     * draw; a model whose texture the raster can't make goes by the walk,
     * which leaves it out and says so */
    walk_refused = 0;
    for (uint32_t i = 0; i < count; i++) {
        const sa_cap_material *made = &materials[i];

        materials[i] = material_as_drawn(&model->mats[i], mode, state, 0);
        cuts[i] = (made->flags & SA_MAT_BLEND) && (made->flags & SA_MAT_TEXTURE) && made->src == SA_BLEND_SRC_ALPHA &&
                  made->dst == SA_BLEND_INV_SRC_ALPHA && made->alpha == 255 && psp_texture_cuts(made->texture);
    }
    if (walk_refused) {
        return walk_model(model, mode, state);
    }
    /* the light in the model's space, by its dots with the rows that carry
     * the model's axes to the view's, as the walk finds it */
    for (int k = 0; k < 3; k++) {
        float length = m[4 * k] * m[4 * k] + m[4 * k + 1] * m[4 * k + 1] + m[4 * k + 2] * m[4 * k + 2];

        light[k] = m[4 * k] * (float)seen[0] + m[4 * k + 1] * (float)seen[1] + m[4 * k + 2] * (float)seen[2];
        longest = length > longest ? length : longest;
    }
    ps2_models_drawn++;
    ps2_models_points += count_points;
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
    if (model->normals) {
        uint32_t spec = count > 0 && (materials[0].flags & SA_MAT_SPECULAR);
        uint32_t pal = (state->palette & 7U) + (spec ? 1U : 0U);

        colors = sa1_live_colors + state->palette * SA_PALETTE_ENTRIES * 2;
        offsets = pal < SA_CAP_PALETTES_COUNT ? sa1_live_colors + pal * SA_CAP_PALETTE_ENTRIES * 2 + 1 : NULL;
        ps2_vu_palette(state->palette | spec << 8, colors, offsets);
    }
    constant = argb(1.0, state->material.r, state->material.g, state->material.b);
    view_id(1);
    psp_draw_screen(sa1_screen.scale_x, sa1_screen.scale_y, sa1_screen.cx, sa1_screen.cy);
    psp_draw_place(m);
    for (uint32_t s = 0; s < model->nbMeshset; s++) {
        const NJS_MESHSET *set = &model->meshsets[s];
        uint32_t named = set->type_matId & MATERIAL_ID;
        const sa_cap_material *material = &materials[named < count ? named : 0];
        int cull = (material->flags & SA_MAT_CULL) != 0;
        int blended = (material->flags & SA_MAT_BLEND) != 0;
        int cut = named < count && cuts[named];
        struct models_set w = {
            .model = model,
            .m = m,
            .count_points = count_points,
            .uv = set->vertuv,
            .env = (material->flags & SA_MAT_ENV) && model->normals,
            .colors = colors,
            .offsets = offsets,
            .unlit_offsets = (material->flags & SA_MAT_UNLIT) && (material->flags & SA_MAT_TEXTURE),
            .shade = {.light = {light[0], light[1], light[2]}, .scale = {1.0F, 1.0F, 1.0F, 1.0F}},
        };
        struct psp_state drawn = {
            .texture = material->flags & SA_MAT_TEXTURE ? material->texture : 0,
            .decal = (material->flags & SA_MAT_DECAL) != 0,
            .clamp_u = (material->flags & SA_MAT_CLAMP_U) != 0,
            .clamp_v = (material->flags & SA_MAT_CLAMP_V) != 0,
            .point = material->filter == SA_FILTER_POINT,
            .depth = (uint8_t)((state->isp & ISP_DEPTH) >> ISP_DEPTH_SHIFT),
            .depth_write = !(state->isp & ISP_Z_WRITE_DISABLE),
            .cut = (uint8_t)cut,
            .blend = (uint8_t)(blended && !cut),
            .src = material->src,
            .dst = material->dst,
            .offset = (material->flags & SA_MAT_OFFSET) != 0,
            .fog = (uint8_t)((state->tsp & TSP_FOG) >> TSP_FOG_SHIFT),
            .flip_u = (material->flags & SA_MAT_FLIP_U) != 0,
            .flip_v = (material->flags & SA_MAT_FLIP_V) != 0,
            /* the texels' density in view space picks the mip level by
             * depth (walk_density); an env map's u and v follow no surface */
            .density = (material->flags & SA_MAT_TEXTURE) && !(material->flags & SA_MAT_ENV) && longest > 0.0F
                           ? walk_density(model, set) / sqrtf(longest)
                           : 0.0F,
        };
        const Sint16 *at = set->meshes;
        struct models_batch batch = {NULL, 0};
        uint32_t corner = 0;

        if (named >= count) {
            continue;
        }
        w.shade.corner_offsets = w.unlit_offsets;
        /* SA1's shade, as the walk's: a meshset under SA_MAT_WHITE leaves
         * the draw's points white for itself and each meshset after it. a
         * point's color is its entry's: white where a meshset that blends
         * is unlit, and once a meshset whitened. one that blends takes its
         * material's alpha. a constant color stands in every point's
         * place, and with no normals each point is white */
        white |= (material->flags & SA_MAT_WHITE) != 0;
        {
            int whiten = white || (blended && (material->flags & SA_MAT_UNLIT));
            uint32_t alpha = blended ? (uint32_t)material->alpha : 255U;

            if (material->flags & SA_MAT_CONSTANT) {
                models_flat(&w.shade, (constant & 0x00ffffffU) | alpha << 24);
            } else if (!model->normals) {
                models_flat(&w.shade, 0x00ffffffU | alpha << 24);
            } else {
                for (int k = 0; k < 3; k++) {
                    w.shade.scale[k] = whiten ? 0.0F : 1.0F;
                    w.shade.add[k] = whiten ? 255.0F : 0.0F;
                }
                w.shade.scale[3] = blended ? 0.0F : 1.0F;
                w.shade.add[3] = blended ? (float)alpha : 0.0F;
            }
        }
        for (uint32_t p = 0; p < set->nbMesh; p++) {
            uint32_t n;
            int reversed;
            int box = 0;

            walk_polygon(set, &at, &n, &reversed);
            if (n < 3) {
                at += n;
                corner += n;
                continue;
            }
            if (cutting) {
                box = models_box(m, points, count_points, at, n);
                ps2_models_boxes++;
                if (box == 1) {
                    at += n;
                    corner += n;
                    continue;
                }
            }
            /* a strip that blends goes on the EE to the picture's sort: one
             * within every side whole, with its depth, which VU1 draws in
             * its place, and one across a side cut */
            if (drawn.blend) {
                models_close(&batch);
                if (n <= WALK_STRIP) {
                    /* the cut takes the strip's turn on its own, as the
                     * walk's does; a kept strip turns by its cull */
                    drawn.cull = (uint8_t)(!cull ? 0 : box == 2 || !reversed ? 1 : 2);
                    ps2_models_ee += n;
                    if (box == 2) {
                        models_strip(strip, &w, at, n, corner);
                        psp_draw_cut(strip, n, reversed, &drawn, eye[2]);
                    } else {
                        struct psp_corner *room = psp_draw_room(n, 1);

                        if (room) {
                            models_strip(room, &w, at, n, corner);
                            psp_draw_later_within(models_depth(m, points, count_points, at, n), &drawn, room, n);
                        }
                    }
                }
                at += n;
                corner += n;
                continue;
            }
            drawn.cull = 0;
            if (box != 2) {
                /* the strip's corners into the batch, a new batch where it's
                 * full, which goes on from the strip's last two corners */
                if (!models_room(&batch, 3, 0, &drawn, &w, at, 0, corner)) {
                    return 1;
                }
                for (uint32_t k = 0; k < n; k++) {
                    if (k >= 2 && !models_room(&batch, 1, 1, &drawn, &w, at, k, corner)) {
                        return 1;
                    }
                    ps2_models_corners++;
                    models_corner(batch.out + 12 * batch.used++, &w, (uint16_t)at[k], corner + k,
                                  models_word(k, cull, reversed));
                }
                at += n;
                corner += n;
                continue;
            }
            /* a strip across a side, by its corners' sides: a triangle
             * within every side goes to VU1 with the rest, one past a side
             * whole goes nowhere, and a run of those across goes on the EE,
             * cut, as a strip of its own */
            if (n <= WALK_STRIP) {
                uint32_t k = 2;
                int run = 0;

                for (uint32_t j = 0; j < n; j++) {
                    uint32_t i = (uint16_t)at[j];

                    sides[j] = (uint8_t)models_sides(m, &points[i < count_points ? i : 0], reach);
                }
                while (k < n) {
                    uint32_t every = sides[k - 2] & sides[k - 1] & sides[k];
                    uint32_t any = sides[k - 2] | sides[k - 1] | sides[k];

                    if (every != 0) {
                        run = 0;
                        k++;
                        continue;
                    }
                    if (any == 0) {
                        if (!run) {
                            if (!models_room(&batch, 3, 0, &drawn, &w, at, k, corner)) {
                                return 1;
                            }
                            for (uint32_t j = k - 2; j < k; j++) {
                                models_corner(batch.out + 12 * batch.used++, &w, (uint16_t)at[j], corner + j,
                                              MODELS_START);
                            }
                        } else if (!models_room(&batch, 1, 1, &drawn, &w, at, k, corner)) {
                            return 1;
                        }
                        ps2_models_corners++;
                        models_corner(batch.out + 12 * batch.used++, &w, (uint16_t)at[k], corner + k,
                                      models_word(k, cull, reversed));
                        run = 1;
                        k++;
                        continue;
                    }
                    /* the run of triangles across, from the one k ends */
                    {
                        uint32_t first = k - 2, end = k;

                        while (end + 1 < n) {
                            uint32_t e = sides[end - 1] & sides[end] & sides[end + 1];
                            uint32_t a = sides[end - 1] | sides[end] | sides[end + 1];

                            if (e != 0 || a == 0) {
                                break;
                            }
                            end++;
                        }
                        models_close(&batch);
                        ps2_models_ee += end - first + 1;
                        models_strip(strip, &w, at + first, end - first + 1, corner + first);
                        drawn.cull = (uint8_t)(cull ? 1 : 0);
                        psp_draw_state(&drawn);
                        /* a run from an odd triangle turns the strip's other way */
                        psp_draw_cut(strip, end - first + 1, (reversed != 0) != ((first & 1) != 0), NULL, eye[2]);
                        drawn.cull = 0;
                        run = 0;
                        k = end + 1;
                    }
                }
            }
            at += n;
            corner += n;
        }
        models_close(&batch);
    }
    return 1;
}
