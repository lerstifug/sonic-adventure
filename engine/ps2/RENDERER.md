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
3. **Textures on the VU1 path** (renderer stage 3 of the models' path):
   UVs, ST and Q from `model.vsm`, and highlights from the palette's offsets.
   It fits the draw list as one more kind of draw.
4. **Mipmaps** (`TEXTURES.md` stage 5), whose levels stream with their
   textures.
5. **480i field rendering**, if the texture room is still short there.

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
