# The PS2's textures: conversion, storage and loading

A plan for replacing the PS2 port's single texture pack (`textures.pak`,
364 MB, one flat 16-bit or 32-bit image per texture, read at each texture's
first draw) with per-file TIM2 textures in the GS's own formats, loaded when
the game itself loads them.

## What the disc holds

A survey of every texture the game's index finds on `orig/`, decoded by the
engine's own PVR reader: **15,947 textures in 978 files**.

- **Files:** about 430 loose `.PVR` files, loaded one texture at a time by
  `njLoadTexture`. The rest are PVM archives, PRS-compressed (`*.PRS`),
  loaded whole by `njLoadTexturePvmMemory`.
- **Pixel formats:** RGB565 12,484, ARGB4444 2,298, ARGB1555 1,142. **No
  palettized PVRs at all**, and no YUV or bump maps.
- **Layouts:** twiddled 8,014, VQ 6,394 (472 of them "small VQ"), rectangle
  577.
- **Mipmaps:** 12,807 (80%). The current pack keeps only the top level.
- **Alpha:** solid 12,661, clear-or-solid 1,095, graded 2,191 (mostly
  ARGB4444: effects, UI and foliage edges).
- **Distinct colours:** 16 or fewer 1,243, 17 to 256 9,699, more than 256
  5,005.
- **Longest side:** 8 to 32 texels 5,312, 64 6,488, 128 2,980, 256 931,
  512 235 (backgrounds, skies, menu screens), 1024 1.

## What each texture becomes

The converter picks, per texture, the smallest GS format that holds it
exactly, and loses precision only where a number says it's invisible:

| Texture | GS format | Loss |
|---|---|---|
| 16 or fewer colours | 4-bit with a 16-entry 32-bit CLUT | none |
| 17 to 256 colours | 8-bit with a 256-entry 32-bit CLUT | none |
| VQ, more than 256 colours | 8-bit, quantized from the VQ book's at most 1024 colours | small; kept only above an error threshold |
| RGB565 or ARGB1555, more than 256 colours | 16-bit (CT16) | none on screen: RGB565's sixth green bit is beyond the 15-bit screen |
| ARGB4444 with graded alpha, more than 256 colours | 32-bit (CT32) | none |

- **The CLUT keeps alpha:** a CLUT texture keeps graded alpha in its CLUT,
  so effects and foliage stay 8-bit.
- **Alpha scale:** alpha is stored at half scale, as the GS counts 0x80 as
  1.0 (the raster does this now).
- **Mipmaps:** a mipmapped PVR keeps its own mip levels, remapped to the top
  level's CLUT where it has one, and drawn with the GS's mipmapping
  (`MIPTBP1`/`MIPTBP2`, LOD from `TEX1`'s K and L).
- **Sides:** sides stay powers of two, as the PVRs already are.

Estimated totals at the GS's power-of-two sides, by use:

| Use | Textures | Disc | Current pack | Plan (top levels) | Mostly |
|---|---|---|---|---|---|
| Action stages | 5,785 | 101 MB | 161 MB | 129 MB | 8-bit 3,246, 16-bit 1,460 |
| Adventure fields | 3,957 | 24 MB | 64 MB | 37 MB | 8-bit 2,646, quantized 841 |
| Chao | 1,327 | 10 MB | 16 MB | 10 MB | 8-bit 880 |
| UI and menus | 1,243 | 65 MB | 78 MB | 63 MB | 8-bit 494, 16-bit 448 |
| Events | 1,155 | 10 MB | 13 MB | 9 MB | 8-bit 757 |
| Bosses | 1,094 | 7 MB | 15 MB | 9 MB | 8-bit 714 |
| Enemies, common objects | 928 | 4 MB | 6 MB | 4 MB | 8-bit 690 |
| Characters | 458 | 2 MB | 2 MB | 1 MB | 8-bit 272, 4-bit 143 |
| **All** | **15,947** | **223 MB** | **355 MB** | **262 MB** | 8-bit 11,513, 16-bit 2,828, 4-bit 1,243, 32-bit 363 |

The total matters less than what one scene keeps in the GS's memory: 2.2 MB
past the screens and the depth buffer. There, 8-bit textures take half the
room and half the upload of today's 16-bit ones, and 4-bit a quarter.
Mipmaps add a third to a mipmapped texture's size, and pay it back in the
GS's texture cache on distant surfaces.

What each use needs:
- **Action stages and adventure fields:** mipmaps, wrap and mirror
  repeats, and highlights (the walk's offset colours).
- **Characters:** small and always resident while the character is loaded.
  Under SA1's palette lighting.
- **UI and menus:** 2D on the screen's own pixels. No mipmaps, clamped, many
  large rectangles with graded alpha. The 512-texel backgrounds show alone
  or with little else.
- **Events:** loaded per cutscene, alongside the stage they play in.
- **Effects and particles:** graded alpha (ARGB4444), blended, so 8-bit CLUT
  textures with alpha.
- **Textures the game makes in memory** (`NJD_TEXATTR_TYPE_MEMORY`): these
  have no file, so they keep today's conversion at run time
  (`psp_texture_texels`).

## How they're stored

**One TIM2 file per name of the original texture files**, in the run
folder's `textures/`, named after the original without its extension:

```
build/engine/ps2/game/textures/BEACH01.tm2       <- SONICADV/BEACH01.PRS (a PVM)
build/engine/ps2/game/textures/SONIC.tm2
build/engine/ps2/game/textures/SCORE_BACK.tm2    <- SCORE_BACK.PRS and SCORE_BACK.PVR together
```

- **It matches how the game loads:** every load names one disc file, so
  each load is one file read, and the mapping to the original is exact.
  Per-level bulk files would fight that: shared archives (characters,
  `OBJ_REGULAR`, the HUD) would be duplicated or need indirection, and one
  changed texture would rebuild a whole level's file.
- **Shared names:** two names belong to both a loose PVR and a PVM.
  `SCORE_BACK.PVR` is one texture `SCORE_BACK.PRS` also holds, under the
  same global index. `CAPTUREBEAM` is a 128×128 texture in both, under two
  global indexes. A name's `.tm2` holds every texture of every file of that
  name, the PVM's first, and a texture two of them hold alike (the same
  global index and texels) once. Loading either file reads the same `.tm2`.
- **TIM2 holds a PVM's many textures in one file**, each picture with its
  own format, CLUT and mipmaps. Each picture's user data carries what the
  game looks textures up by: its global index (GBIX) and its PVR name, plus
  whether its alpha only cuts.
- **The data is aligned for DMA:** pictures and CLUTs are aligned to 128
  bytes (TIM2's alignment flag), so the raster sends texels to the GS
  straight from the file's buffer, with no copy.
- **Debugging and modding:** any TIM2 viewer opens a file. Replacing
  `X.PRS.tm2` changes exactly the textures of `X.PRS`, and the converter
  rebuilds one file when one original changes.

The converter writes `textures/report.csv` beside them: each texture's
source, chosen format and measured error.

## When they're loaded

**Today:** the game loads a PVM into its texture records (the shim) and
nothing reaches the PS2. At each texture's first draw, the walk searches
`textures.bin` on `host:` and reads the texels from the pack. So every
texture's disc read happens in the middle of play (the logs' "the walk made
texture N … at vsync 312").

**Planned:** the texture data arrives exactly when the game loads it.

1. The shim's two loaders, `njLoadTexturePvmMemory` (a PVM) and
   `njLoadTexture` (loose PVRs, or textures made in memory), already end in
   one hook, `sa1_textures_loaded`, with the records made and the file's
   name id.
2. There the PS2 reads that file's `.tm2` whole, in one read, into one
   aligned buffer. It ties each record to its picture by global index and
   name, as `find_place` does now.
3. A draw then finds its texture in memory. No draw reads the disc.
4. The GS's memory stays managed per picture, as now (`gs.cpp`'s VRAM
   manager): a texture uploads from EE memory by DMA when a picture first
   binds it. That costs no disc time, and about 1 or 2 textures a picture
   once a scene is warm.
5. `njReleaseTexture` and `njReleaseTextureAll` free a file's buffer when
   its last record goes.

**Load pauses and loads during play:** most loads come at the game's own
pauses:
- stage, act and character setup (`texture_load_stage`, `texture_load_act`,
  `texture_load_character`)
- the HUD and result screens
- menus, cutscenes and Chao gardens

Some come during play:
- objects that load their own loose PVRs as they spawn, in stages with no
  main texture list (`src/object/common.c`)
- ZERO, the robot chasing Amy (`src/enemy/zero.c`)
- the Tails race opponent (`src/player/race.c`)
- a new landtable's textures (`src/collision/world.c`)

The Dreamcast read the disc at these same moments, and they're small files.
So the PS2 does what the game does: it reads them then, synchronously. The
run log will say how long each texture load took and whether it came in a
load pause, so any real hitch shows up. Only if one does would those loads
move to a background read started ahead of time.

**Resident textures:** every stage start loads the same shared archives
again, though they were loaded before:
- `SONIC` and `SON_EFF` (the character)
- `OBJ_REGULAR` and `EFF_REGULAR` (rings, springs, item boxes, effects)
- `CON_REGULAR` (the HUD)
- `EXTRA`, `HINT` and `EME_KIRAN`

Rather than read them each time, a list (`engine/ps2/resident.txt`, a line
per name with its tier) keeps them in one of two tiers:

- **memory:** the `.tm2` stays in EE memory from its first load to the end
  of the run. A release keeps it, and the next load ties the new records to
  it with no read. EE memory is plentiful: the regular sets take about
  0.9 MB as 8-bit.
- **fixed:** as memory, and its textures also take a fixed place in the
  GS's memory at their first load, outside the VRAM manager's room. They
  are uploaded once and never evicted, never moved, never uploaded again.
  This suits only what's drawn in nearly every picture, because the GS's
  room is 2.2 MB in all. To start with that's the HUD (`CON_REGULAR`, about
  115 KB as 8-bit), and perhaps the font. The VRAM manager already keeps
  what a scene draws often.

A texture is in either tier once, under its one record, however many lists
or files name it: the shim already gives one global index one record.

**Double reading:** the game still reads and PRS-decodes each PVM itself
before `njLoadTexturePvmMemory`, and the PS2 then ignores those texels. A
later step can skip the PVM's read where its `.tm2` exists, building the
records from the TIM2's GBIX and names instead. That saves up to a third of
a second of decoding per large archive at load time.

## Stages, each checked before the next

1. **The converter** (`engine/tools/textures_tm2.cpp`, `engine/ps2/tim2.h`):
   **done.** `make ps2` builds it and runs it from `orig/` into
   `game/textures/`, and the run folder's copy takes only the files that
   changed. It takes 11 seconds.
   - **Output:** 976 files of 15,900 pictures, 256 MB of texels and CLUTs:

     | Format | Pictures |
     |---|---|
     | 8-bit CLUT | 10,104 |
     | 8-bit fitted from VQ | 1,794 |
     | 4-bit CLUT | 801 |
     | 16-bit | 2,838 |
     | 32-bit | 363 |

     4-bit goes only to textures at least 16 texels wide, which the GS's
     4-bit transfers take whole.
   - **Fitting:** about 20 VQ textures fit worse than 40 dB and stayed 16-
     or 32-bit. The farthest kept fit is 40.0 dB.
   - **Checks:** every picture is decoded back and compared with its image.
     Lossless formats differ only by the alpha's halving, and 16-bit by the
     5 bits the GS keeps. A Python reader, independent of the tool, renders
     the files to PNG correctly: the alpha effects, the title screen,
     Emerald Coast's ground, and the worst fit.
   - **The old pack is still built,** since the runtime reads it until
     stage 2.
2. **The loader** (`engine/ps2/textures.cpp`): **done**, together with stage
   3, which it needed.
   - **When:** at `sa1_textures_loaded`, records.c makes every texture of
     the game's load. Each record's file id finds its `.tm2` through
     `textures/files.txt`, which the converter writes, and the file is read
     whole once for the load's list.
   - **No copies:** textures draw from the file's bytes. A file closes when
     its last texture goes, unless `textures/resident.txt` keeps it.
   - **Logging:** each read is logged with its time, and "during play" or
     "at a pause" by the game's own `stage_mode`.
   - **Fallback:** the pack stands in for a texture no `.tm2` holds (none in
     Emerald Coast), or for all textures under `textures pak` in the
     settings.
   - **Why stage 3 came with it:** making every loaded texture at load
     (rather than at first draw), with interim 16-bit copies of the CLUT
     textures, ran the heap out at Emerald Coast's load. Native CLUTs with
     no copies fixed it.
   - **Measured in Emerald Coast:** 37 reads, 4.4 MB in 564 ms, all at load
     pauses. Every loaded texture is held, as on the Dreamcast: 3.2 MB, the
     heap at 10.4 MB of 14 (9.3 MB with the pack, which held only drawn
     textures, as 16-bit).
3. **The raster's CLUT formats** (`gs.cpp`): **done.**
   - **Placement:** 8-bit and 4-bit textures sit with their CLUTs after
     their texels in the GS's memory (`TEX0`'s CBP and CLD). Page and block
     sizes for each format are in `blocks_of`.
   - **Uploads:** texels and CLUT go up from the file by DMA REF.
   - **Mirrors:** a mirrored copy keeps its texture's CLUT.
   - **Freeing:** a file's texture going while a stream may still read it
     returns its file only once the stream is sent.
   - **The fixed tier:** works as described above (`CON_REGULAR`): its
     textures take room from the top of the GS's memory down, at their first
     bind, and the raster keeps them past the game's releases.
4. **Renderer stage 3, textures on the VU1 path:** UVs gathered with each
   corner, ST and Q out of `model.vsm`, highlights from the palette's offset
   half (already in VU1's memory), and the textured material rules of the
   walk (unlit offsets, decal, environment mapping).
5. **Mipmaps:** `MIPTBP1`/`MIPTBP2` per texture, and LOD from Q tuned
   against the Dreamcast's look.
6. **Faster loads:** skip the game's own PVM read and decode where a `.tm2`
   exists.
7. **Only if the log shows hitches:** background reads for the loads that
   happen during play.
