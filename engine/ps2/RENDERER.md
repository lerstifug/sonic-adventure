# The PS2's renderer, built around 32-bit color

A plan for the GS raster (`engine/ps2/gs.cpp`) and the VU1 model path
(`engine/ps2/sa1/models.h`, `engine/ps2/model.vsm`). It replaces the 16-bit
design the raster grew from, which every 32-bit change fought. Textures, their
TIM2 files and their loading are in `TEXTURES.md`.

## Why 32-bit

- **Blends match the Dreamcast.** The PowerVR blends at full precision.
  Over a 16-bit screen, the GS's blends round to 5 bits a channel: faint
  smoke, fog, shadows and fades band and darken, and every blended pixel
  takes the dither.
- **Depth goes to 24 bits.** A 32-bit color buffer takes a 24-bit or 32-bit
  depth buffer: the GS pairs the two by pixel size. 24-bit depth is 256 times
  finer than 16-bit, so far surfaces fight less.
- **No dither on screen.**
- **It's the configuration that's known to work.** 32-bit drawing with
  24-bit depth rendered correctly in PCSX2 and never froze. What froze was
  a copy of each frame into a 16-bit display buffer, made to win back
  texture room; the GS-side cause wasn't found. This plan needs no copy.

24-bit color (CT24) costs the same memory as 32-bit, as the GS stores it in
32-bit words. Its one gain is a known trick, keeping 8-bit textures in each
pixel's spare byte (PSMT8H), which stays an option if the texture room proves
too small.

## The GS's memory

| | Bytes |
|---|---|
| Two 640×480 32-bit frames, flipped at the vertical blank | 2.4 MB |
| 24-bit depth | 1.2 MB |
| Textures | about 0.6 MB, the HUD's fixed area among it |

480i keeps the same layout at 448 lines (0.6 MB more for textures). Drawing
each 224-line field on its own, as many PS2 games did, would halve every
buffer there: a later option.

0.6 MB doesn't hold a scene's textures, and isn't meant to. As in most PS2
games, the GS's memory holds the screens, and textures stream in from main
memory every frame by DMA.

## Stages, each checked before the next

1. **The frame.** Two 32-bit buffers and 24-bit depth. The interrupt at
   the vertical blank flips the display to the buffer just finished, once
   the GS reports it drawn (its FINISH). It only sets the GS's display
   registers, and starts no DMA. Depth spans the full 24 bits. No dither.
   The VRAM manager runs as now in the smaller room, and thrashes until
   stage 2.
2. **The draw list and texture streaming.** The EE records each draw
   rather than sending it at once, then sends a picture in passes. Each
   texture goes up just before the draws that use it, into a ring in the
   texture room, at most once per pass:
   - **opaque:** sorted by texture, then by the rest of their state, so a
     texture's draws run together and its room is free again after them
   - **transparent:** the farthest first, by object as now
   - **2D:** the screen's own pixels, in the game's order

   The HUD keeps its fixed area outside the ring. Measured by bytes
   uploaded a picture, and by the GS's time for them.

   **Done.** Opaque draws record their VIF bytes into a staging buffer, and
   the picture's stream refers to them by REF tags, so nothing is copied.
   At the blends they go sorted by palette, texture, then state; a draw's
   offset pass sorts right behind it. Textures go into a ring below the
   fixed area. `sort 0` in the settings draws at once, for comparison.

   At the start of Emerald Coast, per picture:

   | | Stage 1 | Stage 2 |
   |---|---|---|
   | Textures uploaded | ~820 | ~113 |
   | Bytes uploaded | ~10.4 MB | ~0.89 MB |
   | EE time | ~83 ms | ~74 ms |
   | Heap at its most | 10.4 MB | 8.5 MB |

   What was learned:
   - **The ring did the work.** The old LRU picked among textures all used
     in the same picture, so it put out ones about to be used again. A
     picture binds about 124 distinct textures, and now sends each once,
     which is the floor for a 0.5 MB room. Sorting alone changes little
     here; it pays off with the VU1 path's palettes.
   - **The EE's heap is short.** The heap has about 13.4 MB above the
     program, and the game's loads take most of it. 2.6 MB of staging made
     the game's file reads stall at loads. The stream is now 2 × 128 KB,
     since the staging holds the bulk; the staging is 2 × 384 KB, and a
     picture that outgrows it sends its sorted draws early and draws the
     rest at once.
   - **The EE's time goes elsewhere:** the walk and the blends' triangles
     on the EE, which the VU1 path (stage 3) takes over.
3. **Textures on the VU1 path** (renderer stage 3 of the models' path):
   UVs, ST and Q from `model.vsm`, and highlights from the palette's offsets.
   It fits the draw list as one more kind of draw.

   **Done**, under `models 1`. A corner is three qwords (point and word,
   normal, u and v), and VU1 writes its ST, RGBAQ and XYZF2 over them in
   place, after the GIF tag at the end of the head, so a batch still takes
   80 corners. `model.vsm` also fogs as `strip.vsm` does. A highlight's
   alpha is the offset's brightest channel: a palette entry carries it in
   its fourth word, and a corner carries the unlit words' in its normal's.
   Meshsets that blend go on the EE to the transparency sort, as the walk
   sends them, and models under Ninja's own drawers go by the walk.

   At the start of Emerald Coast, VU1 draws about 14,000 of a picture's
   17,000 triangles, and the EE takes about 66 ms a picture, against 74 ms
   with the walk.
4. **Mipmaps** (`TEXTURES.md` stage 5), whose levels stream with their
   textures.

   **Done.** A texture's levels lie after its texels in the ring, and go
   up with them; `MIPTBP1` and `MIPTBP2` go with its TEX0. The GS picks a
   level by depth alone (log2 of 1/Q, plus TEX1's K), not by how densely
   a surface maps its texture. So each meshset's density, the root of its
   UV area over its area, is measured once and kept by its address
   (`walk_density`), and `draw_of` turns it into K for that draw:
   log2(texels per view unit over the focal length). Draws with no
   density (2D, the screen's own pixels, env maps) draw the top level
   only.

   At the start of Emerald Coast a picture uploads about 1.3 MB (136
   textures) against 0.9 MB (115) without mipmaps, and the EE takes about
   71 ms against 66 ms. Distant ground no longer shimmers. K takes a
   surface as seen head on, so a floor at a grazing angle picks a level
   sharper than the Dreamcast's would; a bias is the knob if it
   shimmers.
5. **480i field rendering**, if the texture room is still short there.

   **Done**, under `video 480i` with `fields 1`. A picture is a field's 224
   lines, 640×224, and the display's frame mode shows it whole in both
   fields. Every place that maps the game's lines to the screen halves them
   past the crop (`y_scale`): the EE's corners, both VU1 programs' heads,
   and the screen's own 2D pixels. Both fields show the same lines, so a
   thin edge doesn't flicker, and the flicker filter stays off. The
   vertical resolution halves.

   The screen's buffers and depth take 1.7 MB instead of 3.4, leaving
   2.4 MB for textures. At the start of Emerald Coast a picture uploads
   about 9 textures (120 KB) against 136 (1.3 MB) at 480p: the scene's
   textures stay in the ring. The EE takes about 69 ms against 71.

   Each field is the same picture: drawing the second field half a line
   lower would give back the vertical resolution, but needs a picture a
   field, 60 a second.

## What carries over

- **The VU1 model path** (`models.h`, `model.vsm`): geometry, culling, and
  SA1's palette lighting.
- **The TIM2 textures:** native 4-bit and 8-bit CLUTs, lent from the loaded
  files, made at the game's own loads.
- **The alpha test for blends:** texels too faint to show are skipped, so
  an effect's clear edges draw nothing.
- **VIF1's ordered stream:** VU1 batches, DIRECT packets, and texels by REF
  tags.
- **No `ExitHandler` in interrupt handlers:** ps2sdk's enables interrupts
  inside the kernel's dispatch.
