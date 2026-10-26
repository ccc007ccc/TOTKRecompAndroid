// Hardware Opus IPC backed by the game's own nn::codec software decoder.
// Protocol/SDK boundary cross-checked against TotK-Native's sdk_opus_decoder.cpp.
#include "../ipc.h"
#include "../hle.h"
#include <stdlib.h>
#include <string.h>

#define OPUS_BAD_ARGUMENT 0x3026fu
#define OPUS_BROKEN_DATA 0x226fu
static const char *const opus_symbols[] = {
    "_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii",
    "_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm",
    "_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE",
    "_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm",
    "_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE"
};

static uint64_t opus_call(int which, const uint64_t *args, unsigned count) {
    Cpu *c = kernel_current_cpu();
    if (!c) FATAL("Opus SDK call outside a guest thread\n");
    uint64_t fn = loader_find_symbol(opus_symbols[which]);
    if (!fn) FATAL("Opus SDK symbol missing: %s\n", opus_symbols[which]);
    Cpu saved = *c;
    memcpy(c->x, args, count * sizeof *args);
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved;
    return result;
}
static uint64_t opus_work_size(uint32_t rate, uint32_t channels) {
    uint64_t args[] = {rate, channels};
    uint64_t size = opus_call(0, args, 2);
    if (!size || size > 16 * 1024 * 1024) FATAL("Opus SDK returned invalid work size %llu\n", (unsigned long long)size);
    return (size + 4095) & ~4095ull;
}

typedef struct OpusState {
    uint8_t *object;  // small decoder bookkeeping in the HLE arena, reclaimed on reboot
    KTransferMem *transfer;  // client's work buffer, retained for the session lifetime
    uint32_t channels;
    pthread_mutex_t lock;
    struct OpusState *next;
} OpusState;
static OpusState *decoders;
static pthread_mutex_t decoder_list_lock = PTHREAD_MUTEX_INITIALIZER;

static void opus_release(OpusState *d) {
    pthread_mutex_lock(&g_klock);
    d->transfer->obj.refs--;
    pthread_mutex_unlock(&g_klock);
    pthread_mutex_destroy(&d->lock);
    free(d);
}
static void opus_close(Service *s) {
    OpusState *d = s->state;
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    uint64_t args[] = {(uint64_t)(uintptr_t)d->object};
    opus_call(2, args, 1);
    pthread_mutex_unlock(&d->lock);
    pthread_mutex_lock(&decoder_list_lock);
    for (OpusState **p = &decoders; *p; p = &(*p)->next)
        if (*p == d) { *p = d->next; break; }
    pthread_mutex_unlock(&decoder_list_lock);
    s->state = NULL;
    opus_release(d);
}
RT_ON_RESET(opus_reset) {
    // Guest threads have stopped; their modules/arena are about to be discarded.
    while (decoders) {
        OpusState *d = decoders;
        decoders = d->next;
        opus_release(d);
    }
}
static bool opus_buffer(const IpcBuf *b) {
    return b && b->size && b->size <= 16 * 1024 * 1024 && b->addr >= GUEST_LO &&
           b->addr < GUEST_HI && b->size <= GUEST_HI - b->addr;
}
static void opus_decoder(Service *s, IpcReq *req, IpcResp *resp) {
    OpusState *d = s->state;
    bool reset_arg = req->cmd == 6 || req->cmd == 8;
    if (req->cmd != 0 && req->cmd != 4 && !reset_arg) FATAL("unsupported Opus decoder command %u\n", req->cmd);
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    if (!d || !opus_buffer(in) || !opus_buffer(out) ||
        (reset_arg && (!req->data_size || req->data[0] > 1)) ||
        (in->addr < out->addr + out->size && out->addr < in->addr + in->size)) {
        resp->result = OPUS_BAD_ARGUMENT;
        return;
    }
    pthread_mutex_lock(&d->lock);
    uint64_t obj = (uint64_t)(uintptr_t)d->object;
    if (reset_arg && req->data[0]) { uint64_t a[] = {obj}; opus_call(4, a, 1); }
    memset(d->object + 0x100, 0, 16);
    uint64_t args[] = {obj, obj + 0x100, obj + 0x108, out->addr, out->size, in->addr, in->size};
    plat_write_prepare(out->addr, out->size);
    uint64_t start = plat_now_ns(), result = opus_call(3, args, 7);
    uint64_t consumed;
    int32_t samples;
    memcpy(&consumed, d->object + 0x100, 8);
    memcpy(&samples, d->object + 0x108, 4);
    if (result) resp->result = OPUS_BROKEN_DATA;
    else if (consumed > in->size || consumed > INT32_MAX || samples < 0 ||
             (uint64_t)samples > out->size / (d->channels * 2))
        FATAL("Opus SDK returned invalid PCM bounds\n");
    else {
        OUT(uint32_t, 0) = (uint32_t)consumed;
        OUT(uint32_t, 4) = (uint32_t)samples;
        OUT(uint64_t, 8) = (plat_now_ns() - start) / 1000;
        resp_size(resp, req->cmd == 0 ? 8 : 16);
    }
    pthread_mutex_unlock(&d->lock);
}

SERVICE("hwopus", hardware_opus) {
    (void)s;
    bool open = req->cmd == 0 || req->cmd == 4;
    bool extended = req->cmd == 4 || req->cmd == 5 || req->cmd == 8;
    if (!open && req->cmd != 1 && req->cmd != 5 && req->cmd != 8) FATAL("unsupported hwopus command %u\n", req->cmd);
    unsigned param_size = extended ? 16 : 8;
    if (req->data_size < param_size + (open ? 4 : 0)) { resp->result = OPUS_BAD_ARGUMENT; return; }
    uint32_t rate = IN(uint32_t, 0), channels = IN(uint32_t, 4);
    if ((rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        !channels || channels > 2 || (extended && (IN(uint32_t, 8) > 1 || IN(uint32_t, 12)))) {
        resp->result = OPUS_BAD_ARGUMENT; return;
    }
    uint64_t size = opus_work_size(rate, channels);
    if (!open) {
        OUT(uint32_t, 0) = (uint32_t)size;
        resp_size(resp, 4);
        LOG("[hwopus] %u Hz, %u channels: work size %llu\n", rate, channels, (unsigned long long)size);
        return;
    }
    pthread_mutex_lock(&g_klock);
    KObject *o = req->ncopy == 1 ? handle_get(req->copy_handles[0]) : NULL;
    KTransferMem *t = o && o->type == KOBJ_TMEM ? (KTransferMem *)o : NULL;
    bool valid = t && t->size == IN(uint32_t, param_size) && t->size >= size;
    if (valid) t->obj.refs++;
    pthread_mutex_unlock(&g_klock);
    if (!valid) { resp->result = OPUS_BAD_ARGUMENT; return; }
    OpusState *d = calloc(1, sizeof *d);
    if (!d) FATAL("Opus session allocation failed\n");
    d->object = hle_alloc(0x200);
    d->transfer = t;
    d->channels = channels;
    pthread_mutex_init(&d->lock, NULL);
    uint64_t args[] = {(uint64_t)(uintptr_t)d->object, rate, channels, t->addr, t->size};
    if (opus_call(1, args, 5)) { opus_release(d); resp->result = OPUS_BAD_ARGUMENT; return; }
    pthread_mutex_lock(&decoder_list_lock);
    d->next = decoders; decoders = d;
    pthread_mutex_unlock(&decoder_list_lock);
    Service *decoder = service_new("IHardwareOpusDecoder", opus_decoder, d);
    decoder->on_close = opus_close;
    ipc_return_object(resp, decoder);
}
