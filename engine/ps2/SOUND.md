# The PS2's sound

A plan for the PS2 port's sound, in stages each checked before the next. It
follows the decisions taken while planning the port's assets: ADX stays ADX,
the sound effects' samples become the SPU2's own ADPCM (VAG), and movies go
through the IPU.

## What the game plays

- **Music and voices** are CRI's ADX: the `.ADX` files and the voices'
  `.AFS` archive. ADX decodes cheaply; a stereo 44.1 kHz stream costs well
  under 1% of the EE.
- **Sound effects** come from the `.MLT` banks of the Dreamcast's sound
  driver. A bank holds instruments, each made of samples in the AICA's
  formats (16-bit or 8-bit PCM, or Yamaha 4-bit ADPCM), with the chip's
  settings for pitch, envelope and loop. The game plays them as notes ("play
  program 12, note 60"), sometimes from small sequences, through the AICA's
  64 voices and its DSP's reverb.
- **The engine** (`engine/pc/sound.cpp`) already plays all of it. The PSP's
  small mixer (`SA_SMALL`) renders each voice, envelope and the DSP in
  software, and the ADX streams from their files.

## Stages

1. **Output: the engine's own render, by audsrv.**
   - A thread of the EE's, one priority above the game's, renders 1,024
     frames at a time with the engine's mixer, as the PSP's window does.
   - ps2sdk's `audsrv` takes each piece at 44.1 kHz, converts it to the
     SPU2's 48 kHz on the IOP, and streams it. The thread waits for
     `audsrv`'s room, so it takes the EE only while it renders.
   - `audsrv.irx` is built into the program (`bin2c`), and loads after the
     BIOS's `LIBSD`.
   - Everything plays, as the engine plays it. The report gives the
     mixer's share of the EE, which is what stage 2 is for.
   - The file client is safe for the sound thread's reads of the ADX
     files: ps2sdk's `fio` serialises its calls (`_fio_io_sema`).
   - **The thread's priority:** the game's thread starts at 1, the highest
     a thread takes, and the EE's kernel runs another of the same priority
     only when the first waits. So the game's thread steps down to 2 and
     the sound's takes 1; at equal priority the sound starved, playing 9 s
     of a 25 s run.
   - **No crossing of IOP calls:** `audsrv_wait_audio` is a call held at
     the IOP until its ring has room. Crossing the game's file calls, it
     froze the game's first file read, every run. The thread now makes only
     short calls (`audsrv_available`, then `audsrv_play_audio`), under
     `fio`'s own lock, and sleeps on the EE between them (`DelayThread`).
   - **Measured:** the mix costs 16% to 24% of the EE in Emerald Coast,
     depending on what plays, so a picture goes from about 37 ms to 44 ms.
2. **Sound effects on the SPU2's voices.**
   - **Samples:** each bank's samples are converted to VAG once, in the
     asset build, as the textures became TIM2. They load into the SPU2's
     2 MB when the game loads the bank.
   - **Notes:** each note becomes one hardware voice, with its pitch,
     volume, pan, loop and envelope set from the instrument's settings. The
     SPU2 decodes and mixes them, so the EE's share drops to a few register
     writes a note.
   - **An IOP module:** `audsrv`'s ADPCM calls set no pitch, envelope or
     key-off, so this needs a small module of the port's own over `libsd`.
   - **Approximations:** the AICA's 64 voices against the SPU2's 48 (a
     voice is stolen past them), its envelope rates against the SPU2's
     ADSR, its DSP's programs against the SPU2's reverb presets, and the
     3D placement (QSound) as volume and pan.
3. **Reverb** by the SPU2's own effect, with a preset chosen per DSP
   program.
4. **Movies:** an SFD file is an MPEG-1 system stream, video plus ADX
   audio. The IPU decodes the video, and the audio goes through the ADX
   path.

## Status

Stage 1 plays, under `sound 1` in the settings, and sounds right: three
runs in a row play throughout, with no stall.

Stage 2's first steps are in:
- **`sa1snd`**, the IOP's module (`engine/ps2/iop/`), is built by `ps2.mk`
  with the IOP's compiler and `iopfixup` as ps2sdk's samples build a
  module, and is the program's own bytes. It serves SIF RPC over `libsd`:
  - **upload:** bytes to the SPU2's memory
  - **frame:** each voice's pitch, ADSR, volume and start, then the voices
    keyed off and on
  - **status:** the voices' envelopes and ends
  - **routing:** each core's voices go to its dry output, and core 1 mixes
    core 0's output
- **The EE's client** (`engine/ps2/spu.cpp`) makes each call short and
  under the file lock, as the sound thread's are.
- **A VAG encoder** (`engine/ps2/vag.h`): for each block, the five
  predictors each at the shift its residuals need, and the least error
  kept.
- **Checked:** a 440 Hz test tone, encoded, uploaded and keyed on voice 24,
  reads back an envelope of 16,383 while it plays and 0 after its key-off,
  and is heard.
- **The banks as VAG, made in the build:**
  - `engine/tools/sounds_vag.cpp` reads every MLT file on the disc and
    writes each programs bank's samples as one file, `game/sound/CRC.vag`,
    named by the CRC32 of the bank's bytes (`engine/ps2/vab.h`). Its
    output: 114 MLT files, 132 banks, 1,848 samples, 24.1 MB of VAG.
  - The game takes a bank's bytes whole from its MLT file, so at a load the
    PS2 takes their CRC32 and reads that file. It streams the VAG to a
    region of the SPU2's memory 16 KB at a time.
  - Converting on the PS2 at load instead took 0.8 s a bank and ran the
    heap out at Emerald Coast's load. From the files, a bank takes 75 to
    200 ms and nearly no memory.
  - **Loops:** the SPU2 loops from a 28-sample block. A sample starts
    later by the samples that bring its loop's start to a block's, and its
    loop repeats until it's a whole count of blocks. A loop that would take
    over 8,192 samples of repeats is stretched to the nearest whole count
    instead, under a block's share of its pitch.
- **The notes on the SPU2's voices** (`engine/ps2/sound_spu.h`):
  - A note whose sample is in the SPU2's memory takes a free voice, of 48.
    Otherwise it plays in the mixer, as does every note once the voices are
    all taken.
  - The render still moves the note's envelope and LFO a block at a time,
    and sends them as the voice's volume and pitch once a render (23 ms).
    The SPU2's own envelope stays full, so no envelope is translated.
  - The render keeps the voice's place by its pitch, to end a sample that
    doesn't loop.
  - The filter goes unheard, as it does from the mixer, and the DSP's
    effects too.
- **Measured:** the mix costs 7.8% of the EE in Emerald Coast, from 16% to
  24% with every voice in the mixer.
- **Routing:** core 0's voices reach the output only as core 1's external
  input, whose volume (AVOL) `audsrv` leaves at 0, so they were silent
  until `sa1snd` set it. Notes take core 1's voices first all the same.
  With `trace 1` the log says, every 5 seconds, how many notes the SPU2's
  voices took and how many of each core's sound.

The effects play from the SPU2's voices in PCSX2. Next: a closer listen
against the PC build (loops, envelopes, balance), then reverb (stage 3).
