# The PS2 at 30 frames a second

A plan for drawing every stage and act at a steady 30 frames a second: a
picture in 33.3 ms or less of the EE. Each step is measured and checked
before the next one starts. 60 frames a second waits until every place holds
30.

The target is PCSX2 first, where the measurements are made. A console, whose
EE misses its caches as PCSX2's doesn't, comes after: each place's margin
under 33 ms then, and step 7, are what it takes.

The renderer it changes is in `RENDERER.md`, the textures and the heap's room
in `TEXTURES.md`.

## Where the time goes

Measured in PCSX2, with the sampling profiler (`sample 1`), the sweep of every
stage and act, and switches that leave the land out (`skip 1`) or every model
(`skip 2`), on the `ps2-prebuilt` branch.

| | A picture | Without the land | Without any model |
|---|---|---|---|
| Emerald Coast, act 1 | 45 ms | 21 to 25 ms | 17 to 20 ms |
| Sky Deck, act 3 | 73 to 78 ms | 37 to 41 ms | under 33 ms |
| Mystic Ruins, act 1 | 88 ms | 23 ms | |

- **The land is the most of it.** Leaving it out saves 22 ms of Emerald
  Coast's picture, 37 of Sky Deck's and 65 of Mystic Ruins's.
- **Most of that is the near plane.** A stage's land models are large, and
  their balls cross the near plane in front of the eye, so the model path
  tests every strip of theirs against the sides, and the EE cuts the
  triangles across and draws them itself, a state of the GS's for each run.
  Of the land's cost in Emerald Coast, the model path's own work is a
  quarter, the EE's cutting and drawing near a third, and the batches'
  heads and states an eighth.
- **The bookkeeping is the smaller part.** A replay of a land model's batches,
  which skips its materials and meshsets, saved 0 to 3%: few land models lie
  within every side.
- **VU1's clipping is the lever.** A prototype that lets VU1's CLIP leave out
  the triangles past a side, and sends the cache's batches across a side to
  VU1 whole, took Emerald Coast to 33 ms (the frame's), Sky Deck to 59 to
  65 ms and Mystic Ruins to 64 ms. It leaves out what it should cut, so the
  ground nearest the eye shows holes.
- **The floor is 8 to 28 ms.** The game's own logic, the sound and the
  pictures' other draws, with no model drawn: the EE's busy time a picture,
  its time less the wait the game paces by (the log's "paced"). 12 ms in
  Emerald Coast, 21 in Sky Deck's third act, 28 in Chaos 0.
- **The heap bounds the caches.** In Mystic Ruins the model cache refuses
  6,700 meshsets in 60 pictures for want of room, and they go by the strips'
  path each picture.

## Can every place reach 30?

The EE's busy time a picture, in ms, at 30 frames a second, by run of 1,500
vsyncs with no input: as it is, with no land (`skip 1`), with no model
(`skip 2`), and with the prototype's CLIP (`vuclip 1`). The land is the first
less the second; the objects, the second less the third. After step 1 is the
prototype's time; the steps after it are estimates: steps 2 and 3 take 35%
off the models' time (the batches' heads and states, and the EE's corners,
by the profile), and step 4 leaves the land 3 ms and a tenth of its time.

| | Now | Floor | Objects | Land | After 1 | After 1 to 3 | After 1 to 4 |
|---|---|---|---|---|---|---|---|
| Emerald Coast, act 1 | 44.5 | 11.8 | 13.5 | 19.2 | 32.5 | 25.3 | 24.3 |
| Emerald Coast, act 2 | 18.8 | 7.8 | 7.6 | 3.4 | 16.0 | 13.1 | 13.1 |
| Windy Valley, act 1 | 40.2 | 14.1 | 16.8 | 9.3 | 33.0 | 26.4 | 26.4 |
| Speed Highway, act 1 | 21.3 | 8.6 | 7.2 | 5.5 | 16.0 | 13.4 | 13.4 |
| Red Mountain, act 1 | 56.4 | 14.3 | 20.1 | 22.0 | 37.9 | 29.6 | 29.6 |
| Sky Deck, act 3 | 72.8 | 21.1 | 24.0 | 27.7 | 61.7 | 47.5 | 41.4 |
| Ice Cap, act 1 | 59.0 | 18.3 | 20.7 | 20.0 | 46.4 | 36.6 | 35.5 |
| Chaos 0 | 59.3 | 28.4 | 14.0 | 16.9 | 46.1 | 39.9 | 39.9 |
| Station Square, act 2 | 49.3 | 7.0* | 7.0 | 35.2 | 26.1 | 19.4 | 15.8 |
| Mystic Ruins, act 1 | 82.2 | 13.8* | 13.8 | 54.5 | 62.4 | 45.4 | 29.3 |

\* The adventure fields' floor doesn't measure (their pacing isn't the
stages'); half their time with no land stands in.

- **Steps 1 to 4 bring seven of the ten under 33 ms**, Red Mountain's first
  act at 30 the nearest. Mystic Ruins takes step 4: its land fills the cache's
  room, and the cache refuses most of it.
- **Three stay over: Sky Deck's third act, Ice Cap's first and Chaos 0.**
  Each needs its floor lower and its objects cheaper, steps 5 and 6.
- **VU1 keeps up.** Its program takes 219 cycles a corner, 24 more to unpack
  a packed one: 40,000 corners in a picture at 30 a second. Sky Deck's third
  act, the most, sends 23,000. The cut adds to that for the few triangles
  across a side.
- **What's in a floor.** Profiled with no model, Chaos 0's 28 ms are the
  game's own logic for 40%; the C library and software floats for 25%, of
  which the engine's motions in doubles (`pose_of`, `blend_transform`,
  `affine_inverse`, which slerp by `acos` and `sin`) near 9%; the raster's
  draws of what isn't a model (its scene's sort of blended triangles) 16%;
  the Dreamcast's FTRV in software (`sa_ftrv`) 8%; Ninja's matrix stack
  (`njPushMatrix`, `njRotateXYZ`, `njInvertMatrix`) near 8%; and the sound 5%.
  Sky Deck's 21 ms are the game's for 55%, `sa_ftrv` 11%.
- **PCSX2 isn't the PS2.** PCSX2 times the EE without its caches' misses; a
  console runs the same code slower, by an amount these measurements can't
  give. The plan aims at PCSX2 first. On a console each place needs a margin
  under 33 ms, which steps 5 to 7 are for.

## The steps

### 0. A baseline and its checks

- Ten places that stand for the rest, by the sweep's slowest and the stages'
  kinds: Emerald Coast's acts 1 and 2, Windy Valley's act 1, Speed Highway's
  act 1, Red Mountain's act 1, Sky Deck's act 3, Ice Cap's act 1, Chaos 0,
  Station Square's act 2, and Mystic Ruins's act 1.
- For each, a run of 1,500 vsyncs with no input, and with the recorded tape
  where one exists: its pictures' times, the EE's share by part (sampling),
  and a screenshot at a fixed vsync.
- A step passes where every place's screenshot matches the baseline's, the
  sweep of every stage and act runs as before, and its timing table is
  written beside the baseline's.

**The baseline**, taken on the `ps2-prebuilt` branch, which draws as `ps2`
does (its CLIP test leaves out nothing while `vuclip` is 0). Its runs, shots
and profiles are in `build/ps2-speed/baseline`, and the scripts that take a
step's in `build/ps2-speed/tools`: `baseline.ps1 -Tag NAME` runs the ten
places and the tape, `sweep.ps1` every stage and act. Busy is the EE's time a
picture less the pacing wait, averaged over the windows after the first; the
shares are of the profiled run's samples, idle among them.

| | Busy | Most | Walk | VU1's batches | Their triangles | Cached corners | EE's corners | EE's cut | Game | Raster | Model path | Libc | Sound | Idle |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Emerald Coast, act 1 | 45.0 | 45.5 | 24.5 | 796 | 14,466 | 11,329 | 2,919 | 162 | 15% | 28% | 22% | 11% | 3% | 14% |
| Emerald Coast, act 2 | 20.3 | 20.3 | 9.0 | 392 | 7,072 | 4,974 | 1,534 | 48 | 14% | 13% | 10% | 8% | 3% | 46% |
| Windy Valley, act 1 | 41.8 | 41.9 | 19.7 | 897 | 14,965 | 12,230 | 2,533 | 94 | 21% | 24% | 19% | 11% | 3% | 15% |
| Speed Highway, act 1 | 21.9 | 21.9 | 9.9 | 356 | 5,317 | 3,085 | 1,638 | 87 | 14% | 16% | 11% | 7% | 3% | 43% |
| Red Mountain, act 1 | 57.0 | 57.4 | 34.2 | 814 | 12,768 | 9,560 | 3,049 | 317 | 13% | 28% | 19% | 16% | 5% | 15% |
| Sky Deck, act 3 | 75.2 | 78.0 | 42.4 | 2,275 | 26,289 | 16,433 | 5,676 | 181 | 17% | 29% | 24% | 13% | 3% | 9% |
| Ice Cap, act 1 | 59.6 | 59.7 | 26.2 | 1,367 | 12,499 | 2,642 | 5,130 | 135 | 16% | 31% | 25% | 9% | 4% | 11% |
| Chaos 0 | 59.5 | 64.1 | 21.5 | 1,488 | 5,374 | 2,702 | 1,041 | 199 | 18% | 29% | 14% | 15% | 4% | 16% |
| Station Square, act 2 | 51.5 | 51.5 | 35.0 | 842 | 6,799 | 5,914 | 836 | 452 | 13% | 36% | 18% | 11% | 3% | 14% |
| Mystic Ruins, act 1 | 83.5 | 83.8 | 66.7 | 1,056 | 15,280 | 8,347 | 6,630 | 722 | 11% | 31% | 28% | 12% | 3% | 11% |

Times are ms a picture; the counts, a picture's, from the raster's last
report. The sweep's 81 runs all ran to their end, but for stage 0, the
practice stage, whose files the disc lacks.

### 1. Clipping on VU1

The prototype's test, finished: VU1 cuts each triangle across a side and
draws the parts, where it now leaves the triangle out.

- **The test**, as the prototype has it: each corner's pixels before the
  divide, against the reach's sides (5 half-frames, inside the GS's 4,096
  pixels), and its depth against the near plane, by CLIP; a triangle any of
  whose three corners lies past one takes the ADC bit, as a culled one does.
- **The cut**: each such triangle not past one side whole is listed as the
  strip goes, its three corners' view-space points, u and v, and colors kept
  beside it; after the strip's XGKICK, each listed triangle is cut against
  the sides it crosses (Sutherland-Hodgman, in view space), its new corners
  projected, lit by their colors' mix, and fogged by their depths, and sent
  as a fan of its own.
- **Room**: a batch's buffer holds the head and 80 corners' three qwords, and
  96 qwords spare: the list and the fans go there, and a batch whose cut
  triangles pass that room is made smaller by the cache. 
- **The EE**: the cache's batches across a side go to VU1 whole, with no
  strip's box tested; the EE's own corners (the strips' path) go to VU1 by
  the same program.
- **Passes** where the ground nearest the eye shows no hole and no crack,
  and the places run near the prototype's times.

### 2. A model's terms once, on VU1

Each batch takes a head of 17 qwords: the matrix, the screen, the light, the
fog and the color's terms, the same for each batch of a model.

- The model's terms go to a part of VU1's memory of their own, once a model,
  and a batch's head holds its material's alone: the shade's scale and add,
  the texture's scale and the GS's tag.
- A cached batch's DMA chain is made once, as the cache keeps it: its head's
  words, the REF of its corners and the MSCAL. A draw writes its tag and its
  state's registers where they change.
- **Passes** where the heads' and states' share of the EE (`apply`,
  `model_head`, `begin_draw`, `room`, `draw_of`, `vu_batch_ref`) halves.

### 3. Every opaque meshset by the cache

With the cut on VU1, the EE makes no corner of an opaque model's.

- The meshsets with a highlight by their corners' own offsets, which the
  cache leaves out, and those an env map lights, go to VU1 too, the env
  map's u and v made there from the normal.
- The EE's strips' path stays for the blends' sort alone.
- **Passes** where `models_strip`, `models_corner` and the EE's draw
  (`send_draw`) fall to a few percent.

### 4. The land built ahead of time

The original idea, now worth its cost: each stage's land as the build makes
it, as its textures are.

- A tool reads each stage's land, its models and their materials, and writes
  a pack for VU1: each chunk's corners packed as the cache packs them, its
  strips joined by material, its ball, and its materials' states.
- The stage reads its packs as it reads its TIM2 files, and the land's draw
  becomes a ball's test and a REF a chunk. The land's moving parts, and any
  model the game changes, stay with the cache.
- The heap's room the cache took for the land goes back to it: Mystic Ruins
  and Sky Deck take the most.
- **Passes** where every stage's land draws as before, and the heavy places
  hold 33 ms.

### 5. The floor

For Chaos 0 and Sky Deck's third act, whose floors take most of a picture.

- **The engine's motions in floats.** `pose_of`, `blend_transform` and
  `affine_inverse` work in doubles, as the PC needs; the EE has no double, and
  its C library's take a hundred instructions an operation. In floats on the
  PS2, as the records already are (`engine/psp/single.h`): near 9% of Chaos
  0's floor. The Dreamcast's own motions were single.
- **The game's math on VU0.** `sa_ftrv` and `sa_fipr`, the Dreamcast's
  matrix and vector instructions, and Ninja's matrix stack, by VU0's macro
  instructions, where the EE's FPU does each sum: 8 to 16% of a floor.
- **The raster's other draws.** The scene's sort of blended triangles, a
  merge sort of records in Chaos 0, by the counting sort the blends already
  take.
- **Passes** where Chaos 0's floor falls under 20 ms, and Sky Deck's under 16.

### 6. The objects' blends on VU1

Ice Cap's and Sky Deck's objects take 20 to 24 ms, the most of it blended
meshsets the EE makes and sorts: the ice, the glass, the effects.

- A blended meshset's batches cached as an opaque one's are, and sorted as
  batches (the cache's `ps2_vu_batch_later`), for those with a highlight by
  offset too, whose offset adds by VU1's second pass.
- **Passes** where the objects' time halves in Ice Cap and Sky Deck.

### 7. And only where a place still passes 33 ms

- **VU1's program, scheduled**: `model.vsm` waits out each step's latency, at
  219 cycles a corner; scheduled, 20 to 30. VU1 limits the picture once the
  EE is fast, and a console needs the margin.
- **Fewer batches**: a model's meshsets of one material joined, at the
  cache's making or the land's build, where SA1 draws 2,000 small ones a
  picture.

## What goes

- The land's replay and the `skip` switches leave with the prototype; the
  replay's gain is step 2's and step 4's.
- The prototype's CLIP test stays, as step 1's start.

## The risks

- **VU1's work is hard to see.** A fault there hangs VIF1 or draws garbage,
  and PCSX2 says little: the stall report (VIF1's last code and the chain's
  last words) and a screenshot a step are the tools.
- **The heap.** Step 4's packs sit beside the game's own models, which it
  reads for collision and motions. They're packed, and read with the stage.
- **The look.** The cut's colors are their corners' mix, where the walk's
  cut on the EE lights each new corner. A highlight or a fog step may show
  at a cut's edge.
