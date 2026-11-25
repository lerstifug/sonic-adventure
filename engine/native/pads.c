/* the first pad's record, which play's live pad or a script of keys fills at
 * each scan. the link's --wrap sends the game's calls of pad_scan to
 * __wrap_pad_scan. its hooks (native.h) may stand in for the keys, and run
 * either side of the game's scan */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "native.h"

void __real_pad_scan(void);

int native_scripted;
static FILE *keys;
static struct native_pad *pad_record;

void native_pad_record(void *pad)
{
    pad_record = pad;
}

/* open the script of keys at path. each line holds a vsync in decimal and the
 * buttons held from it, Shinobi's PDD_DGT bits in hex. a line may go on
 * with the stick's x and y, from -128 to 127. both are 0 where it doesn't. a
 * line of any other form ends the script, and its last pad stays held */
void native_keys_open(const char *path)
{
    if (!(keys = fopen(path, "r"))) {
        fprintf(stderr, "sa1: no keys at %s\n", path);
        exit(2);
    }
    native_scripted = 1;
}

static void next_keys(void)
{
    static unsigned long at;
    static uint32_t held, next;
    static int x, y, next_x, next_y;
    uint32_t last = pad_record->on;
    char line[128];

    while (keys && native_vsyncs >= at) {
        held = next;
        x = next_x;
        y = next_y;
        next_x = next_y = 0;
        if (!fgets(line, sizeof line, keys) ||
            sscanf(line, "%lu %x %d %d", &at, &next, &next_x, &next_y) < 2) {
            fclose(keys);
            keys = NULL;
        }
    }
    pad_record->x1 = (int16_t)x;
    pad_record->y1 = (int16_t)y;
    pad_record->on = held;
    pad_record->off = ~held;
    pad_record->press = held & ~last;
    pad_record->release = last & ~held;
}

void (*native_pad_read)(void *pad);

/* a tape of the pad: the live pad's each change, written as it comes, and a
 * tape read back in the live pad's place, at the vsync each change came
 * at. a line holds the vsync in decimal, the buttons held, Shinobi's
 * PDD_DGT bits in hex, then the sticks' x1 y1 x2 y2 and the triggers' r l.
 * the game counts vsyncs, not time, so a tape plays the run it was taken
 * from again, on any host */
static FILE *taping, *tape;

void native_pads_record(const char *path)
{
    if (!(taping = fopen(path, "w"))) {
        fprintf(stderr, "sa1: can't write the pad's tape %s\n", path);
        return;
    }
    setvbuf(taping, NULL, _IOLBF, 0);
}

void native_pads_replay(const char *path)
{
    if (!(tape = fopen(path, "r"))) {
        fprintf(stderr, "sa1: no tape of the pad at %s\n", path);
        exit(2);
    }
    native_scripted = 1;
}

/* the live pad's change since the last line, to the tape */
static void tape_write(void)
{
    static struct native_pad last;
    static int written;
    const struct native_pad *p = pad_record;

    if (written && p->on == last.on && p->x1 == last.x1 && p->y1 == last.y1 && p->x2 == last.x2 &&
        p->y2 == last.y2 && p->r == last.r && p->l == last.l) {
        return;
    }
    fprintf(taping, "%lu %x %d %d %d %d %u %u\n", native_vsyncs, (unsigned)p->on, p->x1, p->y1, p->x2, p->y2,
            (unsigned)p->r, (unsigned)p->l);
    last = *p;
    written = 1;
}

/* the tape's pad at this vsync into the record */
static void tape_read(void)
{
    static unsigned long at;
    static int ahead;
    static unsigned on, next_on, r, l, next_r, next_l;
    static int x1, y1, x2, y2, nx1, ny1, nx2, ny2;
    uint32_t last = pad_record->on;
    char line[128];

    while (tape) {
        if (!ahead) {
            if (!fgets(line, sizeof line, tape) ||
                sscanf(line, "%lu %x %d %d %d %d %u %u", &at, &next_on, &nx1, &ny1, &nx2, &ny2, &next_r,
                       &next_l) < 2) {
                fclose(tape);
                tape = NULL;
                break;
            }
            ahead = 1;
        }
        if (native_vsyncs < at) {
            break;
        }
        on = next_on;
        x1 = nx1, y1 = ny1, x2 = nx2, y2 = ny2;
        r = next_r, l = next_l;
        nx1 = ny1 = nx2 = ny2 = 0;
        next_r = next_l = 0;
        ahead = 0;
    }
    pad_record->on = on;
    pad_record->off = ~on;
    pad_record->press = on & ~last;
    pad_record->release = last & ~on;
    pad_record->x1 = (int16_t)x1;
    pad_record->y1 = (int16_t)y1;
    pad_record->x2 = (int16_t)x2;
    pad_record->y2 = (int16_t)y2;
    pad_record->r = (uint16_t)r;
    pad_record->l = (uint16_t)l;
}

/* the live pad's record is read once for each vsync, as the Dreamcast's
 * library does. its press then lasts the one vsync, however many times or
 * few the game scans in it. read at each scan, a press stayed in the record
 * until the next scan, and the game, which takes the record's press before
 * it scans, saw the press that closed its pause menu again, and paused */
void native_pads_vsync(void)
{
    if (tape && pad_record) {
        tape_read();
        sa1_chord(pad_record);
        return;
    }
    if (native_pad_read && pad_record) {
        native_pad_read(pad_record);
        sa1_chord(pad_record);
        if (taping) {
            tape_write();
        }
    }
}

void __wrap_pad_scan(void)
{
    if ((native_pad_read || tape) && pad_record) {
        /* the record holds the vsync's read */
    } else if (pad_record && !native_hook_pads()) {
        next_keys();
        sa1_chord(pad_record);
    }
#ifdef SA_SMALL
    /* the trace's word of each press: what the player last did */
    if (native_trace && pad_record && pad_record->press) {
        fprintf(stderr, "sa1: buttons %08x pressed, at vsync %lu\n",
                (unsigned)pad_record->press, native_vsyncs);
    }
#endif
    native_hook_pad_scan(pad_record);
    __real_pad_scan();
    native_hook_pad_scanned();
}
