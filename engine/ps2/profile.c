/* the PS2's sampling profiler, which the settings' sample 1 starts: the EE's
 * timer 1 interrupts about a thousand times a second, and each time the
 * program counter the interrupt stopped, as the kernel gives it, counts in a bucket of
 * 64 bytes of code. the buckets span the program's own code and the
 * console's RAM, where the loader's programs lie. at the run's end the
 * counts go to profile.bin beside the program, which
 * engine/tools/ps2profile.py names by the link's symbols.
 *
 * profile.bin is a head, PROFILE_MAGIC, then the bucket's bytes' log2, then
 * for each span its first address and its count of buckets, then the
 * buckets' counts, 32 bits each, the spans' in order */
#include <kernel.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "psp.h"

#define PROFILE_MAGIC 0x46524250U /* PBRF */
#define BUCKET_SHIFT 6
/* the EE's timer 1, by its registers' addresses: its count, mode and
 * compare value. its clock is the bus's, 147.456 MHz, over 256 */
#define T1_COUNT ((volatile uint32_t *)0x10000800)
#define T1_MODE ((volatile uint32_t *)0x10000810)
#define T1_COMP ((volatile uint32_t *)0x10000820)
#define MODE_BUS_256 2U
#define MODE_ZRET (1U << 6)
#define MODE_CUE (1U << 7)
#define MODE_CMPE (1U << 8)
#define MODE_EQUF (1U << 10)
#define INTERVAL 576U /* a thousandth of a second */

/* the program's code, as gdfs.c knows it: the link gives __executable_start
 * the code's first byte (engine/mk/ps2.mk) and etext its end */
extern char __executable_start[], etext[];

struct span {
    uint32_t first, buckets;
    uint32_t *counts;
};

static struct span spans[2];
static uint32_t samples, outside;
static int handler_id = -1;

/* the kernel's dispatch of an interrupt takes exceptions of its own, so
 * COP0's EPC no longer holds the stopped code's address when the handler
 * runs: AddIntcHandler2 gives it as addr */
static s32 sample(s32 cause, void *arg, void *addr)
{
    uint32_t pc = (uint32_t)(uintptr_t)addr;

    (void)cause;
    (void)arg;
    samples++;
    for (int i = 0; i < 2; i++) {
        uint32_t at = pc - spans[i].first;

        if (spans[i].counts && at < spans[i].buckets << BUCKET_SHIFT) {
            spans[i].counts[at >> BUCKET_SHIFT]++;
            goto counted;
        }
    }
    outside++;
counted:
    *T1_MODE = MODE_BUS_256 | MODE_ZRET | MODE_CUE | MODE_CMPE | MODE_EQUF;
    ExitHandler();
    return -1;
}

static void span_make(struct span *s, uint32_t first, uint32_t bytes)
{
    s->first = first;
    s->buckets = (bytes + (1U << BUCKET_SHIFT) - 1) >> BUCKET_SHIFT;
    s->counts = calloc(s->buckets, sizeof *s->counts);
    if (!s->counts) {
        s->buckets = 0;
    }
}

void ps2_profile_start(void)
{
    span_make(&spans[0], (uint32_t)(uintptr_t)__executable_start,
              (uint32_t)(etext - __executable_start));
    span_make(&spans[1], (uint32_t)(uintptr_t)console_ram, CONSOLE_RAM_SIZE);
    handler_id = AddIntcHandler2(INTC_TIM1, sample, 0, NULL);
    if (handler_id < 0) {
        fprintf(stderr, "sa1: no profile: timer 1's interrupt takes no handler\n");
        return;
    }
    *T1_COUNT = 0;
    *T1_COMP = INTERVAL;
    *T1_MODE = MODE_BUS_256 | MODE_ZRET | MODE_CUE | MODE_CMPE | MODE_EQUF;
    EnableIntc(INTC_TIM1);
    fprintf(stderr, "sa1: the profile samples the EE a thousand times a second\n");
}

void ps2_profile_write(const char *path)
{
    uint32_t head[2] = {PROFILE_MAGIC, BUCKET_SHIFT};
    FILE *out;

    if (handler_id < 0) {
        return;
    }
    DisableIntc(INTC_TIM1);
    *T1_MODE = 0;
    if (!(out = fopen(path, "wb"))) {
        fprintf(stderr, "sa1: can't write the profile at %s\n", path);
        return;
    }
    fwrite(head, sizeof head, 1, out);
    for (int i = 0; i < 2; i++) {
        uint32_t span[2] = {spans[i].first, spans[i].buckets};

        fwrite(span, sizeof span, 1, out);
    }
    for (int i = 0; i < 2; i++) {
        if (spans[i].buckets) {
            fwrite(spans[i].counts, sizeof(uint32_t), spans[i].buckets, out);
        }
    }
    fclose(out);
    fprintf(stderr, "sa1: the profile holds %u samples, %u outside its spans, in %s\n",
            (unsigned)samples, (unsigned)outside, path);
}
