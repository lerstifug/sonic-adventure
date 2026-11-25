/* the IOP's half of the PS2's sound effects (engine/ps2/SOUND.md's stage 2):
 * a server of SIF RPC over libsd, which audsrv has started. the EE's half
 * (engine/ps2/spu.cpp) sends samples to the SPU2's memory, and once a piece
 * of sound each voice's changes and the voices keyed off and on. the calls
 * are engine/ps2/sa1snd.h's */
#include "irx_imports.h"

#include "../sa1snd.h"

IRX_ID("sa1snd", 1, 1);

/* the calls' buffer, a frame's or an upload's, its bytes past the head on
 * the 64 the SPU2's DMA takes */
static uint8_t buffer[sizeof(struct sa1snd_upload)] __attribute__((aligned(64)));
static SifRpcDataQueue_t queue;
static SifRpcServerData_t server;

/* a voice's entry in libsd's words: its core, and its number in it */
static uint16_t voice_of(uint32_t voice)
{
    return (uint16_t)SD_VOICE(voice >= 24 ? 1 : 0, voice % 24);
}

static void upload(const struct sa1snd_upload *u)
{
    if (u->bytes == 0 || u->bytes > SA1SND_CHUNK || u->address < SA1SND_FIRST || u->address + u->bytes > SA1SND_END) {
        return;
    }
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, (u8 *)u->data, (u32 *)u->address, u->bytes);
    sceSdVoiceTransStatus(0, 1);
}

static void frame(const struct sa1snd_frame *f)
{
    uint32_t count = f->count < SA1SND_CHANGES ? f->count : SA1SND_CHANGES;
    int core;

    /* the voices keyed off first, so one keyed again in the same frame
     * starts afresh */
    for (core = 0; core < 2; core++) {
        if (f->off[core] != 0) {
            sceSdSetSwitch((u16)(core | SD_SWITCH_KOFF), f->off[core]);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct sa1snd_voice *c = &f->changes[i];
        uint16_t v;

        if (c->voice >= SA1SND_VOICES) {
            continue;
        }
        v = voice_of(c->voice);
        if (c->what & SA1SND_SET_PITCH) {
            sceSdSetParam((u16)(v | SD_VPARAM_PITCH), c->pitch);
        }
        if (c->what & SA1SND_SET_ENVELOPE) {
            sceSdSetParam((u16)(v | SD_VPARAM_ADSR1), c->adsr1);
            sceSdSetParam((u16)(v | SD_VPARAM_ADSR2), c->adsr2);
        }
        if (c->what & SA1SND_SET_VOLUME) {
            sceSdSetParam((u16)(v | SD_VPARAM_VOLL), c->left);
            sceSdSetParam((u16)(v | SD_VPARAM_VOLR), c->right);
        }
        if (c->what & SA1SND_SET_START) {
            sceSdSetAddr((u16)(v | SD_VADDR_SSA), c->start);
        }
    }
    for (core = 0; core < 2; core++) {
        if (f->on[core] != 0) {
            sceSdSetSwitch((u16)(core | SD_SWITCH_KON), f->on[core]);
        }
    }
}

static void status(struct sa1snd_status *s)
{
    for (uint32_t i = 0; i < SA1SND_VOICES; i++) {
        s->envelope[i] = sceSdGetParam((u16)(voice_of(i) | SD_VPARAM_ENVX));
    }
    s->ended[0] = sceSdGetSwitch(0 | SD_SWITCH_ENDX);
    s->ended[1] = sceSdGetSwitch(1 | SD_SWITCH_ENDX);
}

static void *serve(int call, void *data, int bytes)
{
    (void)bytes;
    switch (call) {
    case SA1SND_PING:
        ((uint32_t *)data)[0] = SA1SND_MAGIC;
        break;
    case SA1SND_UPLOAD:
        upload((const struct sa1snd_upload *)data);
        break;
    case SA1SND_FRAME:
        frame((const struct sa1snd_frame *)data);
        break;
    case SA1SND_STATUS:
        status((struct sa1snd_status *)data);
        break;
    default:
        break;
    }
    return data;
}

/* each core's voices to its dry output, core 0's main volume full, and
 * core 1 taking core 0's output as its input, which its output mixes: the
 * MMIX bits MSNDL and MSNDR (11, 10), and MINL and MINR (7, 6). core 1's
 * other bits stay audsrv's, its stream's */
static void route(void)
{
    sceSdSetSwitch(0 | SD_SWITCH_VMIXL, 0xffffff);
    sceSdSetSwitch(0 | SD_SWITCH_VMIXR, 0xffffff);
    sceSdSetSwitch(1 | SD_SWITCH_VMIXL, 0xffffff);
    sceSdSetSwitch(1 | SD_SWITCH_VMIXR, 0xffffff);
    sceSdSetParam(0 | SD_PARAM_MVOLL, 0x3fff);
    sceSdSetParam(0 | SD_PARAM_MVOLR, 0x3fff);
    /* core 1's volume of its external input, core 0's output */
    sceSdSetParam(1 | SD_PARAM_AVOLL, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_AVOLR, 0x7fff);
    sceSdSetParam(0 | SD_PARAM_MMIX, (u16)(sceSdGetParam(0 | SD_PARAM_MMIX) | 0xc00));
    sceSdSetParam(1 | SD_PARAM_MMIX, (u16)(sceSdGetParam(1 | SD_PARAM_MMIX) | 0xcc0));
}

static void serve_thread(void *arg)
{
    (void)arg;
    route();
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&queue, GetThreadId());
    sceSifRegisterRpc(&server, SA1SND_SERVER, serve, buffer, NULL, NULL, &queue);
    sceSifRpcLoop(&queue);
}

int _start(int argc, char *argv[])
{
    iop_thread_t thread;
    int id;

    (void)argc;
    (void)argv;
    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = serve_thread;
    thread.stacksize = 0x800;
    thread.priority = 40;
    if ((id = CreateThread(&thread)) <= 0 || StartThread(id, NULL) < 0) {
        return MODULE_NO_RESIDENT_END;
    }
    return MODULE_RESIDENT_END;
}
