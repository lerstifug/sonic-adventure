/* the PS2 slice's runner. it starts the game's main beside the program's own
 * file, SONICADV.ELF, and runs it until the game leaves or its count of
 * vsyncs ends. the folder holds what a run reads, as the PSP's does
 * (engine/psp/main.c):
 *
 *   disc/        the disc's files, 1ST_READ.BIN among them
 *   programs/    the build's programs, which loads.c reads
 *   names.bin    the build's names, which symbols.c reads
 *   settings.txt the run's settings, where the player keeps any
 *   card.bin     the memory card's image, which card.c makes where none lies
 *   log.txt      what the run reports
 *
 * the settings are lines NAME VALUE: stage N and act N begin a run in that
 * act as the opening ends, vsyncs N ends the run after that many, with its
 * report, quit N quits the stage after that many vsyncs as the pause menu
 * does, keys FILE plays FILE's script of keys in the pad's place
 * (engine/native/pads.c), trace 1 has the log name each file the game
 * opens and each program it loads, video 480p or 480i sets the screen's
 * mode, 480p by default, deflicker 0 turns 480i's flicker filter off, and
 * dither 0 the dither of the 16-bit screen, vu 0 keeps every strip on the
 * EE, and models 1 draws the models by the PS2's own path, which comes in
 * stages (engine/ps2/sa1/models.h).
 *
 * the runner opens the engine, whose raster is the GS's (gs.cpp): the
 * adapter draws to it as the game calls. its window reads the pads
 * (window.cpp), and has no sound yet. the game reaches some memory by constant address: console_ram holds
 * those bytes (engine/psp/psp.h), and the link defines it
 * (engine/tools/ps2link.py) */
#include <kernel.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"
#include "names.h"
#include "native.h"
#include "psp.h"

#define DISC "disc"
#define SETTINGS "settings.txt"
#define CARD "card.bin"
#define LOG "log.txt"
/* the game's stack, and its threads' over it (engine/native/threads.c) */
#define GAME_STACK_SIZE 0x10000

const char *native_disc = DISC;
int native_target_stage = -1, native_target_act;
struct sa_engine *native_engine;
unsigned long native_vsyncs;
int native_trace;
void (*native_pace)(void);
static unsigned long last_vsync, quit_vsync;
static void (*vsync_function)(void);
static char game_stacks[GAME_STACK_SIZE + NATIVE_THREADS_BYTES]
    __attribute__((aligned(64)));

void stage_end(short end);
void syBtExit(void) __attribute__((noreturn));
/* the screen's mode, which engine/ps2/gs.cpp opens: 480p by default, or
 * 480i, whose flicker filter is on unless the settings turn it off */
int ps2_interlaced = 0;
int ps2_deflicker = 1;
/* dither 0 turns off the GS's dither of the 16-bit screen: gradients then
 * band, where the dither's pattern shows on a sharp display */
int ps2_dither = 1;
/* profile 1: the raster times its strips and binds, and the walk its
 * points' light, one by one */
int ps2_profile = 0;
/* vu 0 draws every strip on the EE, as before VU1 drew the opaque ones */
int ps2_vu = 1;
/* models 1 draws the game's models by the PS2's own path
 * (engine/ps2/sa1/models.h) in the walk's place, stage by stage */
int ps2_models = 0;
/* textures pak takes the textures from the pack, as before the TIM2 files
 * (engine/ps2/textures.cpp) */
int ps2_textures_pak = 0;
/* sort 0 draws each opaque draw as it comes, where by default a picture's
 * wait to go sorted by palette and texture (engine/ps2/RENDERER.md) */
int ps2_sort = 1;
/* the sampling profiler's (engine/ps2/profile.c) */
static int sampling;
void ps2_profile_start(void);
void ps2_profile_write(const char *path);

/* the FPU's FCR31 for the game, as the PSP's runner sets it. the EE always
 * rounds toward zero and flushes a denormal, as the SH-4's FPSCR has the
 * game, so the word changes little there but the cause bits */
#define FCR31_TOWARD_ZERO 1U
#define FCR31_FLUSH (1U << 24)

static void set_fpu(void)
{
    native_fpu_game(FCR31_TOWARD_ZERO | FCR31_FLUSH);
}

/* the shim's heap for Shinobi maps its room, which the slice never asks
 * for (shim.c's syMalloc). the PS2 maps nothing */
void native_map(void *at, size_t size, const char *what)
{
    (void)size;
    fprintf(stderr, "sa1: the PS2 maps no memory for %s at %p\n", what, at);
    exit(2);
}

void native_report(const char *why)
{
    fprintf(stderr, "sa1: %s after %lu vsyncs: ", why, native_vsyncs);
    native_report_state(stderr);
    psp_frame_report();
    fflush(stderr);
}

/* the run's end: it reports where play got to, the hooks hear of the end,
 * and the program sleeps, as a PS2 program with no menu to go back to does */
static void end_run(const char *why) __attribute__((noreturn));

static void end_run(const char *why)
{
    native_fpu_host();
    if (sampling) {
        ps2_profile_write("profile.bin");
    }
    native_report(why);
    native_hook_run_end();
    if (native_play_close) {
        native_play_close();
    }
    /* what the drawer, the scene and the sound took, as the PSP's run
     * reports it */
    psp_draw_report();
    psp_scene_report();
    psp_sound_report();
    fprintf(stderr, "sa1: the run ends\n");
    fflush(stderr);
    for (;;) {
        SleepThread();
    }
}

void syBtExit(void)
{
    end_run("the game left");
}

/* the game's stop at a failure it can't go on from, as dreamcast.h's
 * port_stop says */
void port_stop(const char *what, const char *name, uint32_t number)
{
    native_fpu_host();
    fprintf(stderr, "sa1: the game stopped: %s %s 0x%x after %lu vsyncs\n",
            what, name ? name : "", (unsigned)number, native_vsyncs);
    end_run("the game stopped");
}

/* frame.c's waits call it. native_vsyncs counts the calls, not the PS2's
 * time, so a run decides at the same steps each time */
void native_vsync(void)
{
    native_vsyncs++;
    if (native_pace) {
        native_pace();
    }
    /* the live pad, read once a vsync as the desktop's runner reads it: the
     * game's own scan then takes the vsync's read (engine/native/pads.c) */
    native_pads_vsync();
    if (vsync_function) {
        vsync_function();
    }
    if (native_vsyncs % 600 == 0) {
        native_report("at ten seconds");
    }
    if (native_trace && native_vsyncs % 120 == 0) {
        fprintf(stderr, "sa1: alive at vsync %lu\n", native_vsyncs);
    }
    /* the pause menu's quit, stage.h's STAGE_END_QUIT */
    if (quit_vsync && native_vsyncs == quit_vsync) {
        stage_end(6);
    }
    if (native_vsyncs == last_vsync) {
        end_run("done");
    }
}

void njSetVSyncFunction(void (*function)(void))
{
    vsync_function = function;
}

/* the program's first step, ahead of every constructor's: the FPU takes the
 * host's word, under which no exception is enabled (native.h) */
static void __attribute__((constructor(101))) first_fpu(void)
{
    (void)native_fpu_host();
}

static int take_target(const char *name, const char *value)
{
    if (!strcmp(name, "stage")) {
        native_target_stage = atoi(value);
        return 1;
    }
    if (!strcmp(name, "act")) {
        native_target_act = atoi(value);
        return 1;
    }
    if (!strcmp(name, "vsyncs")) {
        last_vsync = strtoul(value, NULL, 10);
        return 1;
    }
    if (!strcmp(name, "quit")) {
        quit_vsync = strtoul(value, NULL, 10);
        return 1;
    }
    if (!strcmp(name, "keys")) {
        native_keys_open(value);
        return 1;
    }
    if (!strcmp(name, "trace")) {
        native_trace = atoi(value);
        return 1;
    }
    /* video 480p or 480i, and deflicker 1 or 0 for 480i's flicker filter
     * (engine/ps2/gs.cpp) */
    if (!strcmp(name, "video")) {
        ps2_interlaced = !strcmp(value, "480i");
        return 1;
    }
    if (!strcmp(name, "deflicker")) {
        ps2_deflicker = atoi(value);
        return 1;
    }
    if (!strcmp(name, "dither")) {
        ps2_dither = atoi(value);
        return 1;
    }
    if (!strcmp(name, "profile")) {
        ps2_profile = atoi(value);
        return 1;
    }
    if (!strcmp(name, "vu")) {
        ps2_vu = atoi(value);
        return 1;
    }
    if (!strcmp(name, "models")) {
        ps2_models = atoi(value);
        return 1;
    }
    if (!strcmp(name, "textures")) {
        ps2_textures_pak = !strcmp(value, "pak");
        return 1;
    }
    if (!strcmp(name, "sort")) {
        ps2_sort = atoi(value);
        return 1;
    }
    /* sample 1: the sampling profiler (profile.c) */
    if (!strcmp(name, "sample")) {
        sampling = atoi(value);
        return 1;
    }
    return 0;
}

/* words that end the run before the game starts */
static void stand(const char *words) __attribute__((noreturn));

static void stand(const char *words)
{
    fprintf(stderr, "sa1: %s\n", words);
    end_run("the run can't start");
}

/* the adapter finds some of the game's statics by name, in the names the
 * build wrote beside the program. a program without them starts no game */
static void find_statics(void)
{
    char words[160];
    const char *lacking;

    if (native_read_symbols()) {
        stand("can't read names.bin, the build's names, beside the program");
    }
    if ((lacking = sa1_statics_find())) {
        snprintf(words, sizeof words,
                 "names.bin holds no object named %s, a static of the game's "
                 "the program finds by name",
                 lacking);
        stand(words);
    }
}

int main(int argc, char **argv)
{
    char *settings;
    FILE *first;
    FILE *log;

    (void)argc;
    (void)argv;
    (void)native_fpu_host();
    /* the log beside the program, where the folder takes a file. the
     * console's own output keeps the run's words otherwise, as an
     * emulator's log shows them */
    if ((log = fopen(LOG, "w"))) {
        setvbuf(log, NULL, _IOLBF, 0);
        stderr = log;
    }
    fprintf(stderr, "sa1: the program runs\n");
    if ((settings = native_settings_read(SETTINGS))) {
        native_settings_take(settings, SETTINGS, take_target);
        if (native_play_settings) {
            native_play_settings(settings, SETTINGS);
        }
    }
    if (!(first = native_disc_file("1ST_READ.BIN"))) {
        stand("no disc: the folder disc, beside the program, holds the "
              "disc's files, 1ST_READ.BIN among them");
    }
    fclose(first);
    fprintf(stderr, "sa1: the settings and the disc are read\n");
    native_card_open(CARD);
    find_statics();
    /* the engine, whose raster is the GS's (gs.cpp): the adapter draws to
     * it as the game calls */
    if (native_play_open) {
        native_play_open(native_disc, native_scripted);
    }
    fprintf(stderr, "sa1: the engine is open\n");
    set_fpu();
    native_asm_init();
    native_threads_place(game_stacks + GAME_STACK_SIZE);
    native_threads_reserve();
    if (sampling) {
        ps2_profile_start();
    }
    fprintf(stderr, "sa1: the game starts\n");
    native_run_main(game_stacks, GAME_STACK_SIZE);
    end_run("main returned");
}
