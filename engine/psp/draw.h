/* how SA1's adapter draws on the PSP, in C. the PSP has no memory for a
 * picture's records, a client that reads them and a scene made of them, as a
 * host draws: the adapter draws as the game calls it. a model goes by its
 * walk (engine/psp/sa1/walk.h), into the picture's commands, which the
 * raster holds open. a sprite or a run of triangles goes to the PSP's scene
 * (direct.cpp), which makes its corners as a host's does and hands them to
 * the raster at the picture's end, after the models: most of them blend, and
 * what blends goes last, the farthest first */
#ifndef PSP_DRAW_H
#define PSP_DRAW_H

#include <stdint.h>

#include "sa_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a strip's corner: its texture's place in 255ths, its color as RGBA in
 * memory, and its point in the space psp_draw_place's matrix carries to the
 * view's */
struct psp_corner {
    int16_t u, v;
    uint32_t color;
    float x, y, z;
#ifdef _EE
    /* the PS2's raster draws what the PSP's leaves out: the corner's
     * offset color, RGB, which adds to the texel where the state's offset
     * is set, and A, the fog a corner of fog by its points takes. the
     * PSP's engine reads the corner's bytes as they lie, so the field is
     * the PS2's alone */
    uint32_t offset;
#endif
};

/* what the strips after it draw under. texture is the raster's, 0 for none.
 * depth is an SA_DEPTH_*. cull is 0 for both sides of a strip, 1 to drop
 * what turns clockwise on the screen, and 2 for a strip that runs
 * reversed. cut leaves out a pixel whose texel is clear, by the half. blend
 * blends by src and dst, each an SA_BLEND_*, and writes no depth */
struct psp_state {
    uint32_t texture;
    uint8_t decal, clamp_u, clamp_v, point;
    uint8_t depth, depth_write, cull, cut;
    uint8_t blend, src, dst;
#ifdef _EE
    /* the PS2's: the corners' offset colors add to the texel (SA_MAT_OFFSET),
     * the fog by its mode (SA_FOG_*), and the texture mirrored past 1 in u
     * or v (SA_MAT_FLIP_U, SA_MAT_FLIP_V) */
    uint8_t offset, fog, flip_u, flip_v;
    /* the texture's u and v units (255 to its side) its strips span to a
     * unit of view space, which picks its mip level by depth; 0 for none,
     * and no mip levels */
    float density;
#endif
};

/* open a picture's commands, over a screen cleared. commands left open by a
 * picture the game gave no records for end undrawn. it returns 0 where no
 * raster is open */
int psp_picture_open(void);
/* the screen the strips after it project onto: a view's point shows at
 * center + focal * (x, y) / z, in the picture's own 640 by 480 */
void psp_draw_screen(float focal_x, float focal_y, float center_x,
                     float center_y);
/* the matrix that carries the strips after it into view space: Ninja's 16
 * floats, which take a point as a row */
void psp_draw_place(const float *matrix);
void psp_draw_state(const struct psp_state *state);
/* room for a strip's corners, or null where there is none: in the
 * picture's commands, or, for a strip psp_draw_later keeps, later nonzero,
 * in the room the kept strips have */
struct psp_corner *psp_draw_room(uint32_t corners, int later);
/* draw the strip of count corners that lies in psp_draw_room's room */
void psp_draw_strip(const struct psp_corner *corners, uint32_t count);
/* keep that strip for the picture's end, where the kept strips draw after
 * every other, the farthest first: depth is how far its model lies from the
 * eye. it draws under state and under the place and the screen of the
 * call, as a strip that blends must */
void psp_draw_later(float depth, const struct psp_state *state,
                    const struct psp_corner *corners, uint32_t count);
#ifdef _EE
/* as psp_draw_later, for a strip its caller found within every side of the
 * raster's reach: depth is the strip's own, which it sorts by whole */
void psp_draw_later_within(float depth, const struct psp_state *state,
                           const struct psp_corner *corners, uint32_t count);
#endif
/* a strip whose model may reach past the raster's reach of the frame, from
 * the caller's own memory: its triangles, each cut to what lies within. it
 * draws under the state last set, or where later is given stays for the
 * picture's end under it, at depth. reversed is the strip's own turn */
void psp_draw_cut(const struct psp_corner *corners, uint32_t count,
                  int reversed, const struct psp_state *later, float depth);
/* the open picture is dropped, and the screen keeps the picture before */
void psp_picture_drop(void);
/* the game is about to write the loader's work area, where the raster keeps
 * a picture's commands between loads: the engine draws them first */
void psp_draw_yield(void);
/* how far past the frame a strip's corner may lie, as Raster::reach says */
float psp_draw_reach(void);

/* make the raster's texture of the disc's texture of this global index,
 * name and file, as pc::Textures::load finds it. it returns 0 where the
 * disc holds none or no memory does (engine/psp/textures.cpp) */
uint32_t psp_texture_load(uint32_t global_index, const char *name,
                          uint32_t file);
void psp_texture_free(uint32_t texture);
/* make the raster's texture of texels the game made in memory, as a PVR's
 * chunk holds them under its pixel format and layout. 0 where the raster
 * reads no such format or has no memory */
uint32_t psp_texture_texels(uint32_t pixel, uint32_t layout, uint32_t width,
                            uint32_t height, const void *texels,
                            uint32_t bytes);
/* whether the raster's texture's texels are each clear or solid, but for
 * an edge's few between: a triangle of it then cuts at half, and needs no
 * blend, and so no order */
int psp_texture_cuts(uint32_t texture);
/* the PSP's scene (direct.cpp). a picture begins under its view and the
 * background's three colors, and takes each further view by the adapter's
 * number for it, 1 to SA_VIEWS. a sprite's or a run of triangles' texture
 * is the raster's, PSP_NO_TEXTURE for none, and PSP_TEXTURE_FAILED for one
 * the raster couldn't make, whose draw is left out. the picture ends over
 * clear, an ARGB color, and shows, projected as own projects, the view most
 * of its draws lay under, where it has one */
#define PSP_NO_TEXTURE 0xffffffffU
#define PSP_TEXTURE_FAILED 0xfffffffeU
void psp_scene_begin(const sa_view *view, const uint32_t back[3]);
void psp_scene_view(uint32_t id, const sa_view *view);
void psp_scene_sprite(const sa_sprite *sprite);
void psp_scene_triangles(const sa_triangles *triangles, const sa_point *points);
void psp_scene_end(uint32_t clear, const sa_view *own);
/* say on stderr what the scene took at most, and what it left out */
void psp_scene_report(void);
/* the walk's time, for psp_frame_report: around each model it draws */
void psp_walk_begin(void);
void psp_walk_end(void);
/* say on stderr how many pictures the PSP showed since the last call, how
 * long each took, and how much of that waited for the display or went to
 * the walk: what a run on the PSP itself has to tell of its speed */
void psp_frame_report(void);
/* Ninja's full drawer's light of a point, as engine/pc/ninja.h's ninja_lit
 * gives it: material is an sa_material's bytes */
void psp_ninja_lit(const sa_lights *lights, const void *material,
                   const float point[3], const float normal[3], uint32_t *base,
                   uint32_t *offset);
/* say on stderr the most a picture's commands and the textures took */
void psp_draw_report(void);
/* say on stderr how long the sound's render took (engine/psp/window.cpp) */
void psp_sound_report(void);

#ifdef _EE
/* the PS2's own model path (engine/ps2/sa1/models.h): a batch of VU1's
 * (engine/ps2/model.vsm) under state, the place and the screen last given,
 * lit by the palette last sent as shade says. ps2_vu_open gives room for
 * PS2_VU_CORNERS corners, twelve floats each: the point's x, y and z in the
 * model's space, and the corner's word, as model.vsm reads it; the normal's
 * x, y and z, and the most of the corner's offset's three, of 255, where
 * shade takes the corners' offsets; the u and v, 255 to the texture's side,
 * and 0, 0. ps2_vu_close sends the batch of count corners written, or none
 * for fewer than 3. null where no picture is open */
#define PS2_VU_CORNERS 80
struct ps2_vu_shade {
    float light[3]; /* the light in the model's space, to dot with a normal */
    float scale[4]; /* the palette's color's red, green, blue and alpha by */
    float add[4];   /* and what adds to each, of 255 */
    /* a highlight takes each corner's offset, not the palette's */
    int corner_offsets;
};
float *ps2_vu_open(const struct psp_state *state, const struct ps2_vu_shade *shade);
void ps2_vu_close(uint32_t count);
/* the palette a batch lights by: 256 entries, each a color and an offset,
 * ARGB, the colors' every second word from colors and the offsets' from
 * offsets, or none for null. it goes to VU1 where key isn't the one there */
void ps2_vu_palette(uint32_t key, const uint32_t *colors, const uint32_t *offsets);
#endif

#ifdef __cplusplus
}
#endif

#endif
