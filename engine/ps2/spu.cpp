/* the EE's half of the PS2's sound effects on the SPU2's own voices
 * (engine/ps2/SOUND.md's stage 2): it loads the IOP's module
 * (engine/ps2/iop/sa1snd.c) and calls it by SIF RPC, each call a short one
 * under ps2sdk's file lock, as the sound thread's calls of audsrv are
 * (engine/ps2/window.cpp) */
#include <stdio.h>
#include <string.h>

extern "C" {
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
}

#include "sa1snd.h"
#include "spu.h"

extern "C" unsigned char sa1snd_irx[];
extern "C" unsigned int size_sa1snd_irx;
extern "C" int _fio_io_sema;

namespace ps2 {
namespace {

SifRpcClientData_t client __attribute__((aligned(64)));
struct sa1snd_upload upload_buffer __attribute__((aligned(64)));
struct sa1snd_frame frame_buffer __attribute__((aligned(64)));
struct sa1snd_status status_buffer __attribute__((aligned(64)));
uint32_t answer[16] __attribute__((aligned(64)));
bool bound = false;

/* a call to the module, under the file lock */
int call(int which, void *send, int send_bytes, void *receive, int receive_bytes)
{
    WaitSema(_fio_io_sema);
    int result = SifCallRpc(&client, which, 0, send, send_bytes, receive, receive_bytes, nullptr, nullptr);
    SignalSema(_fio_io_sema);
    return result;
}

} // namespace

bool spu_open(const char *&why)
{
    int ret = 0;

    if (bound) {
        return true;
    }
    if (SifExecModuleBuffer(sa1snd_irx, size_sa1snd_irx, 0, nullptr, &ret) < 0 || ret < 0) {
        why = "sa1snd didn't load";
        return false;
    }
    /* the server answers once its thread has registered it */
    for (int tries = 0; tries < 1000; tries++) {
        WaitSema(_fio_io_sema);
        int bind = SifBindRpc(&client, SA1SND_SERVER, 0);
        SignalSema(_fio_io_sema);
        if (bind < 0) {
            why = "sa1snd's server didn't bind";
            return false;
        }
        if (client.server != nullptr) {
            break;
        }
    }
    if (client.server == nullptr) {
        why = "sa1snd's server never answered";
        return false;
    }
    answer[0] = 0;
    call(SA1SND_PING, answer, sizeof answer, answer, sizeof answer);
    if (answer[0] != SA1SND_MAGIC) {
        why = "sa1snd answered wrongly";
        return false;
    }
    bound = true;
    return true;
}

bool spu_ready()
{
    return bound;
}

void spu_upload(uint32_t address, const void *bytes, uint32_t count)
{
    const uint8_t *from = static_cast<const uint8_t *>(bytes);

    for (uint32_t done = 0; bound && done < count;) {
        uint32_t n = count - done < SA1SND_CHUNK ? count - done : SA1SND_CHUNK;
        /* the SPU2's DMA takes 64 bytes at a time: the rest is zeros */
        uint32_t whole = (n + 63) & ~63U;

        upload_buffer.address = address + done;
        upload_buffer.bytes = whole;
        memcpy(upload_buffer.data, from + done, n);
        memset(upload_buffer.data + n, 0, whole - n);
        call(SA1SND_UPLOAD, &upload_buffer, static_cast<int>(SA1SND_HEAD + whole), nullptr, 0);
        done += n;
    }
}

void spu_frame(const struct sa1snd_frame &frame)
{
    if (!bound) {
        return;
    }
    uint32_t count = frame.count < SA1SND_CHANGES ? frame.count : SA1SND_CHANGES;
    int bytes = static_cast<int>(offsetof(struct sa1snd_frame, changes) + count * sizeof(struct sa1snd_voice));

    memcpy(&frame_buffer, &frame, static_cast<size_t>(bytes));
    call(SA1SND_FRAME, &frame_buffer, bytes, nullptr, 0);
}

bool spu_status(struct sa1snd_status &status)
{
    if (!bound) {
        return false;
    }
    call(SA1SND_STATUS, &status_buffer, sizeof status_buffer, &status_buffer, sizeof status_buffer);
    status = status_buffer;
    return true;
}

} // namespace ps2
