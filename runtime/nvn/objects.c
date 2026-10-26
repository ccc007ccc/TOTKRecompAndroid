// NVN objects, builders, and state objects (everything except command recording).
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../kernel.h"
#include "../settings.h"
#include "../framepace.h"
#include "nvn.h"

void renderer_sync(void);

static uint64_t next_debug_id = 1;

// ================================================================== device
// NVNdeviceInfo -> value
static uint32_t device_integer(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 52, [1] = 32,            // API version: BOTW 1.0.0 was built against NVN 52.32 (it needs 52.x, >= 32)
        [2] = 14, [3] = 65536, [4] = 256, [5] = 8, [6] = 16, [7] = 4, [8] = 16, [9] = 32,
        [10] = 16, [11] = 4, [12] = 4, [13] = 4, [14] = 16, [15] = 32, [16] = 32,
        [17] = 256, [18] = 256,        // reserved texture / sampler descriptors
        [19] = 4, [20] = 8, [21] = 0x10000, [22] = 0x1000,
        [23] = 256, [24] = 256, [25] = 4096, [26] = 0x20000, [27] = 16,
        [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
        [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
        [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
        [40] = 1, [41] = 1, [42] = 1,  // min/max filtering, stencil8, ASTC
        [43] = 0x40000, [44] = 15, [45] = 2048,
        [46] = 1, [47] = 5, [48] = 1, [49] = 15,
        // [57]: a linear render target's row alignment (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET: rows of
        // align(width x bytes per texel, this)). BOTW addresses its terrain height tiles (256x256 R16, 128 KB apart)
        // on the CPU with it: 4096 put rows 4 KB apart, reading past each tile - the foot placement got heights of 0
        // (or garbage) and sank Link and the NPCs into the ground (nvn_linear_rt_pitch)
        [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 1, [57] = 128,
        [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
    };
    {   // LARECOMP_DEVINFO=<index>=<value>[+<index>=<value>...]: override (test what the game sizes from them)
        static int n = -1;
        static uint32_t idx[16], val[16];
        if (n < 0) {
            n = 0;
            for (const char *e = getenv("LARECOMP_DEVINFO"); e && *e && n < 16;) {
                char *q;
                idx[n] = (uint32_t)strtoul(e, &q, 0);
                if (*q != '=') break;
                val[n++] = (uint32_t)strtoul(q + 1, &q, 0);
                e = *q == '+' ? q + 1 : q;
                if (!*q || *q != '+') break;
            }
        }
        for (int i = 0; i < n; i++) if (idx[i] == which) return val[i];
    }
    static uint32_t asked[4];  // (log each index the game asks for, once)
    if (which < 128 && !(asked[which / 32] & (1u << (which % 32)))) {
        asked[which / 32] |= 1u << (which % 32);
        LOG("[nvn] device info %u -> %u\n", which, which < sizeof v / sizeof *v ? v[which] : 0);
    }
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) { once = true; backend_init(); }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
// The GPU never reports an error here. BOTW calls this (a thread waiting for GPU errors): each call waits a while
// (100 ms; on the Switch it blocks until an error) and says "none", so such a thread doesn't spin.
NVN(nvnDeviceWaitForError) {
    plat_sleep_until_ns(plat_now_ns() + 100000000ull);
    RET(0);
}
NVN(nvnDeviceSetDebugLabel) { (void)c; }
// NVNdeviceInfo -> value as the NVN 55.15 driver in BOTW 1.8.2's sdk answers (SavageWindRecomp, MIT): a constant
// table, with what a Tegra X1 reports for the per-device entries. BOTW 1.8.2 creates its graphics system only when
// the device reports 55.15 or later, and reads entries past NVN 53's (82, 85, 92: default command, compute and
// queue control memory sizes).
static uint32_t device_integer_v55(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 55, [1] = 15,
        [2] = 14, [3] = 0x10000, [4] = 0x100, [5] = 8, [6] = 0x10, [7] = 4, [8] = 0x10, [9] = 0x20,
        [10] = 0x10, [11] = 4, [12] = 4, [13] = 4, [14] = 0x10, [15] = 0x20, [16] = 0x20,
        [17] = 0x100, [18] = 0x100, [19] = 4, [20] = 8, [21] = 0x200, [22] = 0x100,
        [23] = 256, [24] = 256,                    // scratch memory scale factors (per device)
        [25] = 0x1000, [26] = 0x20000, [27] = 0x10, [28] = 0x600, [29] = 0x400, [30] = 0x40, [31] = 0x600,
        [32] = 0xffff, [33] = 0xffff, [34] = 0xffff, [35] = 8, [36] = 0x100000, [37] = 0x1000, [38] = 0x10,
        [39] = 0x1000, [40] = 1, [41] = 1, [42] = 1,
        [43] = 0x40000,                            // L2 size (per device)
        [44] = 0xf, [45] = 0x800, [46] = 1, [47] = 1, [48] = 0x10, [49] = 5, [50] = 1, [51] = 8, [52] = 8,
        [53] = 4, [54] = 0x20,
        [55] = 256, [56] = 0x20,                   // (per device)
        [57] = 0x80,
        [58] = 0x1000, [59] = 1,                   // memory pool page size, zero-filled unmapped pages (per device)
        [60] = 4,
        [61] = 16384, [62] = 0x8000000, [63] = 2048, [64] = 16384, [65] = 16384,  // texture limits (per device)
        [66] = 1, [67] = 1, [68] = 1, [69] = 1, [70] = 5, [71] = 1, [72] = 1, [73] = 1, [74] = 1, [75] = 1,
        [76] = 1, [77] = 0x10, [78] = 0x400, [79] = 0x20, [80] = 0x1000, [81] = 0x10000, [82] = 0x10000,
        [83] = 0x100, [84] = 0x4000, [85] = 0x40000, [86] = 0x1000, [87] = 1, [88] = 4, [89] = 2, [90] = 0,
        [91] = 0x1000, [92] = 0x4000, [93] = 0x1000, [94] = 0x80, [95] = 0x20, [96] = 0xffff, [97] = 1,
        [98] = 0x20,                               // (per device)
        [99] = 0x100, [100] = 0x10, [101] = 0xffff, [102] = 0xffff, [103] = 0x100, [104] = 0x3fff,
    };
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceGetInteger) {
    uint32_t which = (uint32_t)A(1);
    uint32_t v = device_integer_v55(which);
    if (g_game_version == 160 && which <= 1) v = which ? 311 : 53;  // (v1.6.0 needs NVN 53.x, minor >= 311: its SDK 7.3.2)
    *(int32_t *)AP(2) = (int32_t)v;
}
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    RET(plat_now_ns());
}
NVN(nvnDeviceGetTimestampInNanoseconds) {  // counter.timestamp: Switch GPU ticks (614.4 MHz, as the counters are written)
    if (getenv("LARECOMP_LOG_TS_CALLERS")) {  // (diagnosis) each caller once: who times the GPU (dynamic resolution, 30/60)
        static uint64_t seen[32];
        static int nseen;
        bool found = false;
        for (int i = 0; i < nseen && !found; i++) found = seen[i] == c->x[30];
        if (!found && nseen < 32) {
            seen[nseen++] = c->x[30];
            LOG("[nvn] GetTimestampInNanoseconds called from %llx\n", (unsigned long long)c->x[30]);
        }
    }
    RET((uint64_t)((unsigned __int128)G(uint64_t, A(1), 8) * 625 / 384));
}
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
int nvn_watch_texid(void) {  // LARECOMP_WATCH_HANDLE_TEX=<hex texture id>: handles made / bound with it (diagnosis)
    static int w = -2;
    if (w == -2) w = getenv("LARECOMP_WATCH_HANDLE_TEX") ? (int)strtol(getenv("LARECOMP_WATCH_HANDLE_TEX"), NULL, 16) : -1;
    return w;
}
NVN(nvnDeviceGetTextureHandle) {
    if ((int)(A(1) & 0xFFFFF) == nvn_watch_texid()) {
        LOG("[watch] GetTextureHandle tex %#llx sampler %llu (lr %llx)\n", (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)c->x[30]);
        // (BOTW's eft emitter draw f_70009dc924 -> f_700119992c: x23 the emitter, its +0x1e0 the texture binding with the
        // particle file's bytes at +0x40/42/44 and the sampler object at +0x60; x25 that sampler object)
        static int told;
        if (told++ < 4 && c->x[23] > 0x100000000ull && c->x[25] > 0x100000000ull) {
            const uint8_t *res = (const uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(c->x[23] + 0x1e0);
            const uint8_t *so = (const uint8_t *)(uintptr_t)c->x[25];
            LOG("[watch]   binding %p: +40 %u +42 %u +44 %u +48 %llx +60 %llx | sampler object %p:", (void *)res, *(const uint16_t *)(res + 0x40),
                *(const uint16_t *)(res + 0x42), *(const uint16_t *)(res + 0x44), (unsigned long long)*(const uint64_t *)(res + 0x48),
                (unsigned long long)*(const uint64_t *)(res + 0x60), (void *)so);
            for (int i = 0; i < 0x40; i += 4) LOG(" %08x", *(const uint32_t *)(so + i));
            LOG("\n");
        }
    }
    RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20));
}
NVN(nvnDeviceGetTexelFetchHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
RT_ON_RESET(nvn_device_reset) { window_origin = depth_mode = 0; }  // (the next boot sets its own)
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint64_t nvn_linear_rt_pitch(uint64_t row_bytes) {  // (a linear render target's rows in memory, as the game sees them)
    uint64_t a = device_integer(57);
    return a ? (row_bytes + a - 1) / a * a : row_bytes;
}
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { (void)c; }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
NVN(nvnQueueSubmitCommands) {
    int n = (int)A(1);
    const uint64_t *handles = AP(2);
    extern void nvn_check_handles(Cpu *c, const char *what, int n, const uint64_t *h);  // cmdbuf.c
    nvn_check_handles(c, "SubmitCommands", n, handles);
    extern void nvn_check_list(uint64_t h, int depth);  // cmdbuf.c
    for (int i = 0; i < n; i++) nvn_check_list(handles[i], 0);
    for (int i = 0; i < n; i++) backend_execute(handles[i]);
}
NVN(nvnQueueFlush) { (void)c; }
#ifdef __ANDROID__
bool vk_async_on(void);
void vk_async_drain(void);
uint64_t vk_async_fence(uint64_t guest_sync);
void vk_async_wait_id(uint64_t id);
void kernel_blocking_begin(void);
void kernel_blocking_end(void);
NVN(nvnQueueFinish) {
    (void)c;
    kernel_blocking_begin();  // (a long wait: core 0 to the game's other threads, kernel.c)
    if (vk_async_on()) vk_async_drain();  // (the render thread's translation first: vulkan/async.c)
    renderer_sync();
    kernel_blocking_end();
}
#else
NVN(nvnQueueFinish) { (void)c; renderer_sync(); }
#endif

static NvnSync *sync_of(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}
uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);
#ifdef __ANDROID__
NVN(nvnQueueFenceSync) {
    NvnSync *s = sync_of(A(1));
    s->signaled = 1;
    if (vk_async_on()) s->lseq = vk_async_fence(A(1));  // (its submission is known once the render thread gets there)
    else s->seq = renderer_fence_seq();
    extern uint64_t renderer_present_count(void);
    s->frame = renderer_present_count();
}
#else
NVN(nvnQueueFenceSync) { NvnSync *s = sync_of(A(1)); s->signaled = 1; s->seq = renderer_fence_seq(); }
#endif
void nvn_sync_set_seq(uint64_t guest, uint64_t seq) { NvnSync *s = sync_of(guest); s->signaled = 1; s->seq = seq; }
NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnSync))); RET(1); }
NVN(nvnSyncFinalize) { free(HOST(NvnSync, A(0))); SET_HOST(A(0), 0); }
NVN(nvnSyncSetDebugLabel) { (void)c; }
void renderer_flush(void);
NVN(nvnSyncWait) {  // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    static int flush = -1;
    static uint64_t n;
    if (flush < 0) flush = getenv("LARECOMP_SYNC_FLUSH") ? 1 : 0;
    if (getenv("LARECOMP_LOG_SYNC") && n++ % 60 == 0) LOG("[nvn] SyncWait #%llu (lr %llx)\n", (unsigned long long)n, c->x[30]);
    static int whole = -1;  // LARECOMP_SYNC_ALL=1: old behaviour, wait for all submitted work
    if (whole < 0) whole = getenv("LARECOMP_SYNC_ALL") ? 1 : 0;
#ifdef __ANDROID__
    uint64_t t0 = plat_now_ns();
    kernel_blocking_begin();  // (a long wait: core 0 to the game's other threads, kernel.c)
    if (vk_async_on()) {  // the render thread's translation of the fence first (vulkan/async.c)
        if (flush || whole) vk_async_drain();
        else vk_async_wait_id(sync_of(A(0))->lseq);
    }
    uint64_t t1 = plat_now_ns();
#endif
    if (flush) renderer_flush();
    else if (whole) renderer_sync();
    else renderer_wait_seq(sync_of(A(0))->seq);  // only the submission that holds this fence
#ifdef __ANDROID__
    kernel_blocking_end();
    // LARECOMP_LOG_SYNC: waits over a millisecond in the world (frame 1500 on, 40 of them) - how old the fence was,
    // the waits for its translation and its GPU work
    static int log = -1, logged;
    if (log < 0) log = getenv("LARECOMP_LOG_SYNC") != NULL;
    extern uint64_t renderer_present_count(void);
    if (log && plat_now_ns() - t0 > 1000000 && renderer_present_count() > 1500 && logged++ < 40) {
        LOG("[nvn] SyncWait (lr %llx): fence from %lld frames ago, waited %.2f ms for the render thread, %.2f ms for the GPU\n",
            (unsigned long long)c->x[30], (long long)(renderer_present_count() - sync_of(A(0))->frame), (t1 - t0) / 1e6,
            (plat_now_ns() - t1) / 1e6);
    }
#endif
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) { G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0; }
NVN(nvnEventBuilderSetStorage) { G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2); }
NVN(nvnEventBuilderGetStorage) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 8)); }
static uint64_t event_addr(uint64_t ev) { return G(uint64_t, ev, 0); }
NVN(nvnEventInitialize) {
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) { RET(*(uint32_t *)(uintptr_t)event_addr(A(0))); }
NVN(nvnEventSignal) { *(uint32_t *)(uintptr_t)event_addr(A(0)) = (uint32_t)A(2); }
NVN(nvnEventGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnEventGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }

// ================================================================== memory pools
// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = calloc(1, sizeof *p);
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    if (ENV("LARECOMP_LOG_POOLS"))
        LOG("[nvn] pool %llx storage=%llx size=%llx flags=%x\n", A(0), p->storage, p->size, p->flags);
    SET_HOST(A(0), p);
    RET(1);
}
NVN(nvnMemoryPoolFinalize) { free(HOST(NvnPool, A(0))); SET_HOST(A(0), 0); }
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN(nvnMemoryPoolMap) { RET(HOST(NvnPool, A(0))->storage); }
NVN(nvnMemoryPoolGetBufferAddress) { RET(HOST(NvnPool, A(0))->storage); }  // GPU address == CPU address
NVN(nvnMemoryPoolGetSize) { RET(HOST(NvnPool, A(0))->size); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
    backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
NVN(nvnMemoryPoolMapVirtual) {
    // BOTW maps its whole 120 MiB virtual texture heap onto one physical pool before creating textures. GPU
    // addresses in this runtime are CPU pointers: a contiguous whole-pool alias needs no allocation or copy.
    // Sparse/partial remapping needs a separate address translation layer; never pretend it succeeded.
    typedef struct { uint64_t pool, physical_offset, virtual_offset, size; uint32_t storage_class, pad; } Mapping;
    NvnPool *dst = HOST(NvnPool, A(0));
    const Mapping *maps = AP(2);
    uint64_t base = 0, covered = 0;
    if (!A(1)) { RET(1); return; }
    bool valid = dst && (dst->flags & 0x200) && maps;
    for (uint64_t i = 0; valid && i < A(1); i++) {
        const Mapping *m = &maps[i];
        NvnPool *src = m->pool ? HOST(NvnPool, m->pool) : NULL;
        valid = src && src->storage && m->size && m->virtual_offset == covered &&
                m->physical_offset <= src->size && m->size <= src->size - m->physical_offset &&
                covered <= dst->size && m->size <= dst->size - covered;
        if (!valid) break;
        uint64_t addr = src->storage + m->physical_offset;
        if (!i) base = addr;
        else if (addr != base + covered) { valid = false; break; }
        covered += m->size;
    }
    if (!valid || covered != dst->size) {
        LOG("[nvn] MapVirtual %llx: unsupported non-contiguous/partial mapping (%llu requests)\n", A(0), A(1));
        RET(0);
        return;
    }
    dst->storage = base;
    if (ENV("LARECOMP_LOG_POOLS"))
        LOG("[nvn] MapVirtual %llx: %llx bytes aliased at %llx\n", A(0), covered, base);
    RET(1);
}

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = next_debug_id++;
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) { backend_pool_written(G(uint64_t, A(0), 0) + A(1), A(2)); }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize2D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) {
    // Three low nibbles are height/depth/width. The upper bits and second word are not layout flags.
    // Encode presence separately: an explicit all-zero layout is not the driver's default layout.
    uint32_t layout = A(1) ? *(const uint32_t *)AP(1) & 0xfff : 0;
    if ((layout & 15) > 5 || ((layout >> 4) & 15) > 5 || (layout >> 8))
        FATAL("unsupported packaged texture layout %#x\n", layout);
    TB(uint64_t, 0x50) = A(1) ? (uint64_t)layout + 1 : 0;
}
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) {
    uint64_t encoded = TB(uint64_t, 0x50);
    if (A(1)) *(uint32_t *)AP(1) = encoded ? (uint32_t)encoded - 1 : 0;
    RET(encoded != 0);
}
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(0); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) {  // 1D array: height is the layer count
        t->layers = G(uint32_t, b, 0x14);
    }
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    if (getenv("LARECOMP_LOG_BHDIFF") && t->depth <= 1) {  // (diagnosis) where the old block height rule differed
        uint32_t hb = (t->height + t->fmt->bh - 1) / t->fmt->bh, gobs = (hb + 7) / 8, o = 0;
        while ((1u << o) < gobs && o < 4) o++;
        if (o != t->block_height_log2) {
            static int n;
            if (n++ < 300) LOG("[nvn] bh differs: %ux%u layers %u fmt %#x levels %u: %u (old %u) packaged %llx\n", t->width,
                               t->height, t->layers, G(uint32_t, b, 0x20), t->levels, t->block_height_log2, o,
                               (unsigned long long)G(uint64_t, b, 0x50));
        }
    }
    // BNTX supplies both block height and block depth, including explicit zero values.
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl) {
        uint32_t layout = (uint32_t)pl - 1;
        t->block_height_log2 = layout & 15;
        t->block_depth_log2 = (layout >> 4) & 15;
    }
    if (pl && getenv("LARECOMP_LOG_PACKAGED")) {
        static int n;
        if (n++ < 4000) LOG("[nvn] packaged layout %#x fmt %#x %ux%ux%u lv %u -> bh %u bd %u\n", (uint32_t)pl - 1,
                           G(uint32_t, b, 0x20), t->width, t->height, t->depth, t->levels, t->block_height_log2, t->block_depth_log2);
    }
}

static bool is_linear(uint64_t b) { return (G(uint32_t, b, 8) & 0x10) || G(uint32_t, b, 0xC) == 10; }

static uint64_t builder_storage_size(uint64_t b) {
    if (G(uint32_t, b, 0xC) == 10) {  // buffer texture
        return (uint64_t)G(uint32_t, b, 0x10) * nvn_format_info(G(uint32_t, b, 0x20))->bpb;
    }
    if (is_linear(b)) return G(uint64_t, b, 0x30) * G(uint32_t, b, 0x14);
    TexLayout t = {0};
    builder_layout(b, &t);
    return tegra_total_size(&t);
}
NVN(nvnTextureBuilderGetStorageSize) { RET(builder_storage_size(A(0))); }
NVN(nvnTextureBuilderGetStorageAlignment) { RET(is_linear(A(0)) ? 32 : 512); }

static NvnTexture *texture_from_builder(uint64_t b) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->pool = pool ? HOST(NvnPool, pool) : NULL;
    t->pool_offset = G(uint64_t, b, 0x40);
    t->addr = t->pool ? t->pool->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    t->debug_id = next_debug_id++;
    return t;
}

// Texture objects and their owners. An NVN texture object is a description over pool memory, and BOTW treats it so:
// it copies objects (agl::TextureSampler keeps a shallow copy, registers it with a view and finalizes the copy on its
// own), initializes an object again without finalizing it, finalizes one twice, and creates thousands with the same
// description (each would be a VkImage and an upload of its own). Each nvnTextureInitialize makes the object's guest
// address an owner of a host texture - one with the same description is shared - and only that address's finalize (or
// its next initialize) drops the reference: a copy's finalize, or a second one, changes nothing.
// Under g_live_lock: the owner table (open addressing, a freed slot keeps a tombstone), the description hash (by
// storage address) and the list of all textures.
enum { OWNER_SLOTS = 1 << 18, DESC_BUCKETS = 1 << 16 };
#define OWNER_GONE 1ull
typedef struct { uint64_t guest; NvnTexture *t; } TexOwner;
static TexOwner g_owner[OWNER_SLOTS];
static int g_owner_used, g_owner_gone;
static NvnTexture *g_desc_hash[DESC_BUCKETS];
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnTexture *g_all_tex;
uint64_t g_tex_stat_init, g_tex_stat_reinit, g_tex_stat_reused, g_tex_stat_fin_owner, g_tex_stat_fin_copy, g_tex_stat_reg;

static uint32_t owner_hash(uint64_t g) { return (uint32_t)((g >> 3) * 0x9E3779B97F4A7C15ull >> 46) & (OWNER_SLOTS - 1); }
static uint32_t desc_hash(uint64_t addr) { return (uint32_t)((addr >> 9) * 0x9E3779B97F4A7C15ull >> 48) & (DESC_BUCKETS - 1); }
static NvnTexture *owner_take(uint64_t g) {  // removes and returns the texture g owns (NULL: g owns none)
    uint32_t i = owner_hash(g);
    for (int n = 0; n < OWNER_SLOTS && g_owner[i].guest; n++, i = (i + 1) & (OWNER_SLOTS - 1))
        if (g_owner[i].guest == g) {
            NvnTexture *t = g_owner[i].t;
            g_owner[i].guest = OWNER_GONE;
            g_owner[i].t = NULL;
            g_owner_used--, g_owner_gone++;
            return t;
        }
    return NULL;
}
static void owner_put(uint64_t g, NvnTexture *t) {  // (g owns nothing: owner_take first)
    if ((g_owner_used + g_owner_gone + 1) * 4 > OWNER_SLOTS * 3) {  // tombstones: rebuild the table in place
        TexOwner *keep = malloc(sizeof(TexOwner) * (size_t)(g_owner_used ? g_owner_used : 1));
        if (!keep) FATAL("nvn: out of memory rebuilding the texture owners\n");
        int k = 0;
        for (int i = 0; i < OWNER_SLOTS; i++) if (g_owner[i].guest > OWNER_GONE) keep[k++] = g_owner[i];
        memset(g_owner, 0, sizeof g_owner);
        g_owner_gone = 0;
        for (int j = 0; j < k; j++) {
            uint32_t i = owner_hash(keep[j].guest);
            while (g_owner[i].guest) i = (i + 1) & (OWNER_SLOTS - 1);
            g_owner[i] = keep[j];
        }
        free(keep);
        if ((g_owner_used + 1) * 4 > OWNER_SLOTS * 3) FATAL("nvn: more than %d texture objects\n", OWNER_SLOTS * 3 / 4);
    }
    uint32_t i = owner_hash(g);
    while (g_owner[i].guest > OWNER_GONE) i = (i + 1) & (OWNER_SLOTS - 1);
    if (g_owner[i].guest == OWNER_GONE) g_owner_gone--;
    g_owner[i].guest = g;
    g_owner[i].t = t;
    g_owner_used++;
}

static bool texture_same_desc(const NvnTexture *a, const NvnTexture *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return a->addr == b->addr && a->pool_offset == b->pool_offset && a->target == b->target && a->flags == b->flags &&
           a->width == b->width && a->height == b->height && a->depth == b->depth &&
           a->levels == b->levels && a->format == b->format && a->samples == b->samples &&
           a->ds_mode == b->ds_mode && a->stride == b->stride &&
           memcmp(a->swizzle, b->swizzle, 4) == 0 &&
           a->layout.block_height_log2 == b->layout.block_height_log2 &&
           a->layout.block_depth_log2 == b->layout.block_depth_log2;
}
// A guest texture object (192 opaque bytes): our texture @0, OBJ_TAG @8, the object's own swizzle @12, debug ID @16
#define OBJ_TAG 0x5a575331u
static const uint8_t *obj_swizzle(uint64_t g) {
    return G(uint32_t, g, 8) == OBJ_TAG ? (const uint8_t *)(uintptr_t)(g + 12) : NULL;
}
static uint64_t obj_debug_id(uint64_t g) { return G(uint32_t, g, 8) == OBJ_TAG ? G(uint64_t, g, 16) : HOST(NvnTexture, g)->debug_id; }
// The same image: the description but its swizzle, which only says how it is read. BOTW 1.8.2 draws its minimap
// into a 288x288 target and samples it through a second object over the same memory with alpha ONE: as two
// textures, the second was an image of its own, never drawn (the map layer of the minimap was missing). Each guest
// object keeps its own swizzle and debug ID in itself (obj_swizzle, obj_debug_id), copies of it too: register_texture
// gives an entry a view with that swizzle, and nvnTextureCompare tells such objects apart (the game's UI draws masks into
// an R8 target and samples them through a second object, white with the mask as alpha; it registers a texture only
// when no registered one compares equal: told they were the same, it sampled the mask through the first object,
// opaque - black boxes around the stamina wheel, the button prompts and the place names).
static bool texture_same_storage(const NvnTexture *a, const NvnTexture *b) {
    if (!a || !b) return false;
    NvnTexture x = *a;
    memcpy(x.swizzle, b->swizzle, 4);
    return texture_same_desc(&x, b);
}
static NvnTexture *texture_find_same(const NvnTexture *t) {
    if (!t->addr) return NULL;
    for (NvnTexture *e = g_desc_hash[desc_hash(t->addr)]; e; e = e->hnext)
        if (texture_same_storage(e, t)) return e;
    return NULL;
}
static void texture_track(NvnTexture *t) {  // a new host texture: no references yet
    t->refs = 0;
    t->prev = NULL;
    t->next = g_all_tex;
    if (g_all_tex) g_all_tex->prev = t;
    g_all_tex = t;
    if (t->addr) {
        NvnTexture **b = &g_desc_hash[desc_hash(t->addr)];
        t->hnext = *b;
        *b = t;
    }
}

// Descriptor registration copies a texture's description on NVN. Our descriptors reference the host object,
// so they must keep it alive after TextureFinalize. Every dropped reference also waits for pending command lists
// (render targets and copies name textures directly, and the render thread translates a frame behind the game).
enum { RETIRE_FRAMES = 4 };
typedef struct { NvnTexture *t; uint64_t frame; } RetiredTexture;
static RetiredTexture *g_retired;
static int g_nretired, g_cap_retired;
static void texture_release(NvnTexture *t) {  // g_live_lock; render work drained by the backend
    if (--t->refs) return;
    backend_texture_destroyed(t);
    if (t->prev) t->prev->next = t->next;
    else g_all_tex = t->next;
    if (t->next) t->next->prev = t->prev;
    if (t->addr)
        for (NvnTexture **p = &g_desc_hash[desc_hash(t->addr)]; *p; p = &(*p)->hnext)
            if (*p == t) { *p = t->hnext; break; }
    free(t);
}
static void texture_retire(NvnTexture *t) {
    extern uint64_t renderer_present_count(void);
    uint64_t now = renderer_present_count();
    pthread_mutex_lock(&g_live_lock);
    int k = 0;
    for (int i = 0; i < g_nretired; i++) {
        if (g_retired[i].frame + RETIRE_FRAMES <= now) {
            texture_release(g_retired[i].t);
        } else {
            g_retired[k++] = g_retired[i];
        }
    }
    g_nretired = k;
    if (g_nretired == g_cap_retired) {
        g_cap_retired = g_cap_retired ? g_cap_retired * 2 : 256;
        g_retired = realloc(g_retired, g_cap_retired * sizeof *g_retired);
        if (!g_retired) FATAL("nvn: out of memory retiring textures\n");
    }
    g_retired[g_nretired].t = t, g_retired[g_nretired].frame = now;
    g_nretired++;
    pthread_mutex_unlock(&g_live_lock);
}
static void texture_drop_owner(NvnTexture *t) {
    pthread_mutex_lock(&g_live_lock);
    if (!--t->owners) t->finalized = true;
    pthread_mutex_unlock(&g_live_lock);
    texture_retire(t);
}
RT_ON_RESET(textures_reset) {
    // main.c has already stopped guest threads and reset the renderer.
    while (g_all_tex) {
        NvnTexture *next = g_all_tex->next;
        free(g_all_tex);
        g_all_tex = next;
    }
    memset(g_owner, 0, sizeof g_owner);
    g_owner_used = g_owner_gone = 0;
    memset(g_desc_hash, 0, sizeof g_desc_hash);
    free(g_retired);
    g_retired = NULL;
    g_nretired = g_cap_retired = 0;
}

// (debugging, runtime/hle/movie.c) the live textures whose storage overlaps [lo, hi): how the game reads a buffer
void nvn_log_textures_over(uint64_t lo, uint64_t hi) {
    pthread_mutex_lock(&g_live_lock);
    int n = 0;
    for (NvnTexture *t = g_all_tex; t && n < 8; t = t->next)
        if (t->addr && t->addr < hi && t->addr + tegra_total_size(&t->layout) > lo) {
            n++;
            LOG("[nvn]   texture %ux%u fmt %#x flags %#x target %u at %llx (+%#llx) size %#llx stride %llu block %u\n",
                t->width, t->height, t->format, t->flags, t->target, (unsigned long long)t->addr,
                (unsigned long long)(t->addr - lo), (unsigned long long)tegra_total_size(&t->layout),
                (unsigned long long)t->stride, t->layout.block_height_log2);
        }
    if (!n) LOG("[nvn]   no texture over %llx-%llx\n", (unsigned long long)lo, (unsigned long long)hi);
    pthread_mutex_unlock(&g_live_lock);
}
void nvn_texture_cpu_written(uint64_t addr, uint64_t size) {
    if (!size || addr > UINT64_MAX - size) return;
    pthread_mutex_lock(&g_live_lock);
    for (NvnTexture *t = g_all_tex; t; t = t->next)
        if (t->addr && t->addr < addr + size && addr < t->addr + tegra_total_size(&t->layout))
            __atomic_add_fetch(&t->cpu_write_epoch, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_live_lock);
}

// LARECOMP_WATCH_TEXADDR=<hex>: every texture created over that storage address, and its use as a render target or
// a copy's ends (vulkan/exec.c) - who writes memory that a texture object reads (aliasing between descriptions)
uint64_t nvn_watch_texaddr(void) {
    static uint64_t a = ~0ull;
    if (a == ~0ull) a = getenv("LARECOMP_WATCH_TEXADDR") ? strtoull(getenv("LARECOMP_WATCH_TEXADDR"), NULL, 16) : 0;
    return a;
}

NVN(nvnTextureInitialize) {
    // An object initialized again without nvnTextureFinalize (BOTW streams textures so): its previous texture goes,
    // as at a finalize - unless the description is the same.
    g_tex_stat_init++;
    NvnTexture *t = texture_from_builder(A(1));
    if (nvn_watch_texaddr() && t->addr <= nvn_watch_texaddr() && nvn_watch_texaddr() < t->addr + tegra_total_size(&t->layout))
        LOG("[watch] init %llx: %ux%ux%u fmt %#x flags %#x target %u levels %u at %llx (pool offset %llx, samples %u, "
            "ds %u, stride %llu, swizzle %u %u %u %u, block %u/%u) (lr %llx)\n", (unsigned long long)A(0),
            t->width, t->height, t->depth, t->format, t->flags, t->target, t->levels, (unsigned long long)t->addr,
            (unsigned long long)t->pool_offset, t->samples, t->ds_mode, (unsigned long long)t->stride, t->swizzle[0],
            t->swizzle[1], t->swizzle[2], t->swizzle[3], t->layout.block_height_log2, t->layout.block_depth_log2,
            (unsigned long long)c->x[30]);
    {  // LARECOMP_WATCH_READ_TEX=<w>x<h>x<d>: who reads the storage of the first texture of that shape (dispatch.c)
        static int ww = -1, wh, wd, wf = -1;
        if (ww < 0) {
            ww = 0;
            const char *e = getenv("LARECOMP_WATCH_READ_TEX");
            if (e) sscanf(e, "%dx%dx%d:%x", &ww, &wh, &wd, &wf);  // (each texture of that shape [and format], up to 16: dispatch.c)
        }
        if (ww >= 0 && (ww || wh || wd) && (!ww || (int)t->width == ww) && (!wh || (int)t->height == wh) &&
            (!wd || (int)t->depth == wd) && (wf < 0 || (int)t->format == wf) && tegra_total_size(&t->layout) >= 0x10000) {  // (0: any; not the small ones: pages shared with the heap)
            extern void rt_range_watch(uint64_t lo, uint64_t hi);
            LOG("[rwatch] texture %llx %ux%ux%u fmt %#x at %llx size %#llx\n", (unsigned long long)A(0), t->width,
                t->height, t->depth, t->format,
                (unsigned long long)t->addr, (unsigned long long)tegra_total_size(&t->layout));
            rt_range_watch(t->addr, t->addr + tegra_total_size(&t->layout));
        }
    }
    pthread_mutex_lock(&g_live_lock);
    NvnTexture *old = owner_take(A(0));
    NvnTexture *same = old && texture_same_storage(old, t) ? old : texture_find_same(t);
    uint8_t swizzle[4];
    memcpy(swizzle, t->swizzle, 4);
    uint64_t debug_id = t->debug_id;
    if (same) {
        if (ENV("LARECOMP_LOG_TEXALIAS") && memcmp(same->swizzle, swizzle, 4)) {  // shared with an object of another swizzle
            static int n;
            if (n++ < 60)
                LOG("[alias] %llx shares %ux%u fmt %#x at %llx: swizzle %u%u%u%u, the image's %u%u%u%u (refs %u, lr %llx)\n",
                    (unsigned long long)A(0), t->width, t->height, t->format, (unsigned long long)t->addr, swizzle[0],
                    swizzle[1], swizzle[2], swizzle[3], same->swizzle[0], same->swizzle[1], same->swizzle[2],
                    same->swizzle[3], (unsigned)same->refs, (unsigned long long)c->x[30]);
        }
        free(t);
        t = same;
        g_tex_stat_reused++;
    } else {
        if (ENV("LARECOMP_LOG_TEXALIAS"))  // the same memory and size as a live texture, yet another image: why
            for (NvnTexture *e = g_desc_hash[desc_hash(t->addr)]; e; e = e->hnext)
                if (e->addr == t->addr && e->width == t->width && e->height == t->height) {
                    static int n;
                    if (n++ < 40)
                        LOG("[alias] %ux%u at %llx: fmt %#x/%#x flags %#x/%#x target %u/%u levels %u/%u depth %u/%u "
                            "samples %u/%u ds %u/%u stride %llu/%llu pool off %llx/%llx block %u/%u (lr %llx)\n",
                            t->width, t->height, (unsigned long long)t->addr, e->format, t->format, e->flags, t->flags,
                            e->target, t->target, e->levels, t->levels, e->depth, t->depth, e->samples, t->samples,
                            e->ds_mode, t->ds_mode, (unsigned long long)e->stride, (unsigned long long)t->stride,
                            (unsigned long long)e->pool_offset, (unsigned long long)t->pool_offset,
                            e->layout.block_height_log2, t->layout.block_height_log2, (unsigned long long)c->x[30]);
                    break;
                }
        texture_track(t);
    }
    t->refs++;
    t->owners++;
    t->finalized = false;
    owner_put(A(0), t);
    pthread_mutex_unlock(&g_live_lock);
    if (old) {
        g_tex_stat_reinit++;
        static uint64_t n;
        if (ENV("LARECOMP_LOG_TEXINIT") && n++ < 20) LOG("[nvn] TextureInitialize %llx again: previous texture retired\n", (unsigned long long)A(0));
        texture_drop_owner(old);
    }
    if ((t->flags & 2) && !same)
        LOG("[nvn] video texture %p: %ux%u fmt %#x flags %#x target %u addr %llx stride %llu\n", t, t->width, t->height,
            t->format, t->flags, t->target, t->addr, t->stride);
    SET_HOST(A(0), t);
    G(uint32_t, A(0), 8) = OBJ_TAG;
    memcpy(AP(0) + 12, swizzle, 4);
    G(uint64_t, A(0), 16) = debug_id;
    G(uint64_t, A(0), 24) = G(uint64_t, A(1), 0x38);  // preserve this object's pool, even when its image is shared
    RET(1);
}
NVN(nvnTextureFinalize) {
    pthread_mutex_lock(&g_live_lock);
    NvnTexture *t = owner_take(A(0));
    pthread_mutex_unlock(&g_live_lock);
    if (t) {
        g_tex_stat_fin_owner++;
        texture_drop_owner(t);
    } else if (HOST(NvnTexture, A(0))) {
        g_tex_stat_fin_copy++;  // a copy of an object, or an object finalized again
        static int n;
        if (ENV("LARECOMP_LOG_TEXINIT") && n++ < 20)
            LOG("[nvn] TextureFinalize %llx: not an initialized object (%p), ignored (lr %llx)\n", (unsigned long long)A(0),
                HOST(void, A(0)), (unsigned long long)c->x[30]);
    }
    SET_HOST(A(0), 0);
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
#define TEX HOST(NvnTexture, A(0))
NVN(nvnTextureGetFlags) { RET(TEX->flags); }
NVN(nvnTextureGetTarget) { RET(TEX->target); }
NVN(nvnTextureGetWidth) { RET(TEX->width); }
NVN(nvnTextureGetHeight) { RET(TEX->height); }
NVN(nvnTextureGetDepth) { RET(TEX->depth); }
NVN(nvnTextureGetLevels) { RET(TEX->levels); }
NVN(nvnTextureGetFormat) { RET(TEX->format); }
NVN(nvnTextureGetSamples) { RET(TEX->samples); }
NVN(nvnTextureGetSwizzle) {
    const uint8_t *sw = obj_swizzle(A(0));
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = sw ? sw[i] : TEX->swizzle[i];
}
NVN(nvnTextureGetDepthStencilMode) { RET(TEX->ds_mode); }
NVN(nvnTextureGetStride) { RET(TEX->stride); }
NVN(nvnTextureGetTextureAddress) { RET(TEX->addr); }
NVN(nvnTextureGetMemoryPool) { RET(G(uint64_t, A(0), 24)); }
NVN(nvnTextureGetMemoryOffset) { RET(TEX->pool_offset); }
NVN(nvnTextureGetStorageSize) { RET(tegra_total_size(&TEX->layout)); }
NVN(nvnTextureGetDebugID) { RET(obj_debug_id(A(0))); }
NVN(nvnTextureGetViewOffset) {
    NvnTexture *t = TEX;
    const NvnTextureView *v = A(1) ? AP(1) : NULL;
    uint32_t level = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t layer = v && (v->setmask & 2) ? v->min_layer : 0;
    if (level >= t->layout.levels || layer >= t->layout.layers)
        FATAL("texture view offset outside storage: mip %u layer %u\n", level, layer);
    uint64_t offset = tegra_layer_size(&t->layout) * layer + tegra_level(&t->layout, level).offset;
    if (getenv("LARECOMP_LOG_TEXOFFSET")) {
        static int n;
        if (n++ < 80) LOG("[nvn] texture view offset %ux%u f%#x mip %u layer %u -> %#llx\n",
                         t->width, t->height, t->format, level, layer, (unsigned long long)offset);
    }
    RET(offset);
}
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(0); }
NVN(nvnTextureCompare) {  // (each object's own description: two of another swizzle may share one texture)
    const uint8_t *s0 = obj_swizzle(A(0)), *s1 = obj_swizzle(A(1));
    NvnTexture *t0 = HOST(NvnTexture, A(0)), *t1 = HOST(NvnTexture, A(1));
    RET(s0 && s1 ? texture_same_storage(t0, t1) && !memcmp(s0, s1, 4) : texture_same_desc(t0, t1));
}
NVN(nvnTextureFlushTexels) { backend_pool_written(TEX->addr, tegra_total_size(&TEX->layout)); }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views: an NvnTextureView (nvn.h) packed into the guest's 16-byte NVNtextureView. Clearing NVN 53's 0x28
// bytes here zeroed the next sead heap block's list link in BOTW 1.8.2, and its free list then looped.
#define TV ((NvnTextureView *)AP(0))
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, sizeof(NvnTextureView)); }
NVN(nvnTextureViewSetLevels) { TV->setmask |= 1; TV->base_level = (uint8_t)A(1); TV->num_levels = (uint8_t)A(2); }
NVN(nvnTextureViewSetLayers) { TV->setmask |= 2; TV->min_layer = (uint16_t)A(1); TV->num_layers = (uint16_t)A(2); }
NVN(nvnTextureViewSetFormat) { TV->setmask |= 4; TV->format = (uint16_t)A(1); }
NVN(nvnTextureViewSetSwizzle) {
    TV->setmask |= 8;
    for (int i = 0; i < 4; i++) TV->swizzle[i] = (uint8_t)A(1 + i);
}
NVN(nvnTextureViewSetDepthStencilMode) { TV->setmask |= 16; TV->ds_mode = (uint8_t)A(1); }
NVN(nvnTextureViewSetTarget) { TV->setmask |= 32; TV->target = (uint8_t)A(1); }
NVN(nvnTextureViewGetLevels) { *(int32_t *)AP(1) = TV->base_level; *(int32_t *)AP(2) = TV->num_levels; RET(TV->setmask & 1); }
NVN(nvnTextureViewGetLayers) { *(int32_t *)AP(1) = TV->min_layer; *(int32_t *)AP(2) = TV->num_layers; RET(!!(TV->setmask & 2)); }
NVN(nvnTextureViewGetFormat) { *(uint32_t *)AP(1) = TV->format; RET(!!(TV->setmask & 4)); }
NVN(nvnTextureViewGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TV->swizzle[i]; RET(!!(TV->setmask & 8)); }
NVN(nvnTextureViewGetDepthStencilMode) { *(uint32_t *)AP(1) = TV->ds_mode; RET(!!(TV->setmask & 16)); }
NVN(nvnTextureViewGetTarget) { *(uint32_t *)AP(1) = TV->target; RET(!!(TV->setmask & 32)); }
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), sizeof(NvnTextureView)) == 0); }
// (From SavageWindRecomp, MIT; a smaller layout fits the older NVN versions too.)

// CPU texture storage transfers. In particular, TOTK reads packaged BC4 data
// back into linear blocks before assembling its ambient-occlusion atlas.
// GPU-written texture invalidation/readback remains a separate backend concern.
static void texture_transfer(Cpu *c, uint64_t row_stride, uint64_t image_stride, bool write) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    const NvnTextureView *v = A(1) ? AP(1) : NULL;
    const int32_t *r = AP(2);
    uint32_t level = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t layer = v && (v->setmask & 2) ? v->min_layer : 0;
    uint64_t stride = (t->flags & 0x10) ? t->stride : 0;
    int32_t array_region[6];
    if (t->target == 3 && r) {  // 1D arrays address layers through y/height, not z/depth.
        if (r[2] || r[5] != 1 || t->layout.fmt->bh != 1) FATAL("invalid 1D-array %sTexels region\n", write ? "Write" : "Read");
        memcpy(array_region, r, sizeof array_region);
        array_region[1] = 0; array_region[4] = 1; array_region[2] = r[1]; array_region[5] = r[4];
        image_stride = row_stride ? row_stride : (uint64_t)((r[3] + t->layout.fmt->bw - 1) / t->layout.fmt->bw) * t->layout.fmt->bpb;
        r = array_region;
    }
    uint8_t *storage = (uint8_t *)(uintptr_t)t->addr;
    bool ok = write ? tegra_write_region(&t->layout, level, layer, r, storage, stride, AP(3), row_stride, image_stride)
                    : tegra_read_region(&t->layout, level, layer, r, storage, stride, AP(3), row_stride, image_stride);
    if (!ok) FATAL("invalid %sTexels region at mip %u layer %u\n", write ? "Write" : "Read", level, layer);
    if (write) backend_pool_written(t->addr, tegra_total_size(&t->layout));
    static unsigned counts[2];
    if (__atomic_fetch_add(&counts[write], 1, __ATOMIC_RELAXED) < 16 || getenv("LARECOMP_TRACE_TEXELS"))
        LOG("[nvn] %sTexels %ux%u f%#x flags %#x @%llx mip %u layer %u region %d,%d,%d %dx%dx%d linear %llx strides %llu/%llu\n",
            write ? "Write" : "Read", t->width, t->height, t->format, t->flags, (unsigned long long)t->addr,
            level, layer, r[0], r[1], r[2], r[3], r[4], r[5], (unsigned long long)A(3),
            (unsigned long long)row_stride, (unsigned long long)image_stride);
}
NVN(nvnTextureWriteTexels) { texture_transfer(c, 0, 0, true); }
NVN(nvnTextureWriteTexelsStrided) { texture_transfer(c, A(4), A(5), true); }
NVN(nvnTextureReadTexels) { texture_transfer(c, 0, 0, false); }
NVN(nvnTextureReadTexelsStrided) { texture_transfer(c, A(4), A(5), false); }

// ================================================================== samplers
// SamplerBuilder: the game's NVNsamplerBuilder is only 64 bytes in NVN 55 (BOTW 1.8.2 builds one on the stack
// right below its saved frame pointer), so the builder state is packed to fit; an NvnSamplerDesc (72 bytes) after the
// device pointer overwrote the caller's frame. (SavageWindRecomp, MIT; it fits the older versions' larger builder.)
typedef struct {
    uint64_t device;
    uint8_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
} NvnSamplerBuilderState;
_Static_assert(sizeof(NvnSamplerBuilderState) <= 64, "NVNsamplerBuilder is 64 bytes");
#define SD ((NvnSamplerBuilderState *)AP(0))
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerBuilderState *d = SD;
    uint64_t device = d->device;
    memset(d, 0, sizeof *d);
    d->device = device;
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  // REPEAT
    d->lod_min = 0; d->lod_max = 1000.0f; d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1); SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) {
    SD->wrap[0] = (uint32_t)A(1); SD->wrap[1] = (uint32_t)A(2); SD->wrap[2] = (uint32_t)A(3);
    if (getenv("LARECOMP_LOG_WRAP")) {  // each distinct (modes, caller) once (diagnosis)
        static uint64_t seen[256]; static int ns;
        uint64_t k = (A(1) & 0xF) | (A(2) & 0xF) << 4 | (A(3) & 0xF) << 8 | (uint64_t)c->x[30] << 12;
        int i = 0;
        while (i < ns && seen[i] != k) i++;
        if (i == ns && ns < 256) {
            seen[ns++] = k;
            LOG("[nvn] SetWrapMode %llu %llu %llu (thread %d, lr %llx; callers", (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (int)gettid(), (unsigned long long)c->x[30]);
            uint64_t fp = c->x[29];  // frame records: [previous fp, return address]
            for (int d = 0; d < 8 && fp && (fp & 7) == 0; d++) {
                LOG(" %llx", (unsigned long long)((const uint64_t *)(uintptr_t)fp)[1]);
                fp = ((const uint64_t *)(uintptr_t)fp)[0];
            }
            LOG(")\n");
        }
    }
}
NVN(nvnSamplerBuilderSetLodClamp) { SD->lod_min = AF(0); SD->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1); SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SD->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SD->reduction = (uint32_t)A(1); }
NVN(nvnSamplerBuilderSetLodSnap) { (void)c; }
NVN(nvnSamplerBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SD->min_filter; *(uint32_t *)AP(2) = SD->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SD->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SD->lod_min; *(float *)AP(2) = SD->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SD->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SD->compare_mode; *(uint32_t *)AP(2) = SD->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SD->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SD->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

static NvnSamplerDesc sampler_desc_from_builder(const NvnSamplerBuilderState *b) {
    NvnSamplerDesc d = {0};
    d.min_filter = b->min_filter; d.mag_filter = b->mag_filter;
    for (int i = 0; i < 3; i++) d.wrap[i] = b->wrap[i];
    d.compare_mode = b->compare_mode; d.compare_func = b->compare_func; d.reduction = b->reduction;
    d.lod_min = b->lod_min; d.lod_max = b->lod_max; d.lod_bias = b->lod_bias; d.max_aniso = b->max_aniso;
    memcpy(d.border, b->border, sizeof d.border);
    return d;
}

// An NVNsampler holds a pointer to its description, and the game copies sampler objects by value (registering the
// copy later): descriptions are shared per distinct setting and never freed, so a copy made before the original was
// finalized still names its own settings (a malloc'd description per object left the copy a dangling pointer: the
// Shrine of Resurrection's walkway was clamped instead of repeated in some runs - SavageWindRecomp, MIT).
static pthread_mutex_t sampler_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnSamplerDesc *sampler_intern(const NvnSamplerDesc *want) {
    static NvnSamplerDesc **tab;
    static size_t n, cap;
    pthread_mutex_lock(&sampler_lock);
    NvnSamplerDesc *d = NULL;
    for (size_t i = 0; i < n && !d; i++)
        if (!memcmp(tab[i], want, offsetof(NvnSamplerDesc, backend))) d = tab[i];
    if (!d) {
        if (n == cap) { cap = cap ? cap * 2 : 64; tab = realloc(tab, cap * sizeof *tab); }
        d = malloc(sizeof *d);
        *d = *want;
        tab[n++] = d;
    }
    pthread_mutex_unlock(&sampler_lock);
    return d;
}

NVN(nvnSamplerInitialize) {
    NvnSamplerDesc want = sampler_desc_from_builder(AP(1));
    NvnSamplerDesc *d = sampler_intern(&want);
    if (getenv("LARECOMP_LOG_SMPREG"))
        LOG("[nvn] SamplerInitialize object %llx wrap %u %u %u min %u (old host %p, lr %llx, thread %d)\n", (unsigned long long)A(0), d->wrap[0], d->wrap[1],
            d->wrap[2], d->min_filter, (void *)HOST(NvnSamplerDesc, A(0)), (unsigned long long)c->x[30], (int)gettid());
    SET_HOST(A(0), d);
    // The guest object holds the settings too, after our pointer: games compare sampler objects - BOTW's agl reuses
    // the pool slot of an equal one it already registered; with only stale bytes there, the eft's mirrored lens-streak
    // sampler matched the clamped default and took its slot (the hard-edged streaks and haze). The packed builder state
    // (56 bytes after its device pointer): 64 in all, NVN 55's sampler object (96 in older NVN).
    memcpy(AP(0) + 8, (const uint8_t *)AP(1) + 8, sizeof(NvnSamplerBuilderState) - 8);
    RET(1);
}
NVN(nvnSamplerFinalize) { SET_HOST(A(0), 0); }  // (the description is shared: never freed)
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP HOST(NvnSamplerDesc, A(0))
NVN(nvnSamplerGetMinMagFilter) { *(uint32_t *)AP(1) = SMP->min_filter; *(uint32_t *)AP(2) = SMP->mag_filter; }
NVN(nvnSamplerGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SMP->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { *(float *)AP(1) = SMP->lod_min; *(float *)AP(2) = SMP->lod_max; }
NVN(nvnSamplerGetLodBias) { RETF(SMP->lod_bias); }
NVN(nvnSamplerGetCompare) { *(uint32_t *)AP(1) = SMP->compare_mode; *(uint32_t *)AP(2) = SMP->compare_func; }
NVN(nvnSamplerGetBorderColor) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColori) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColorui) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { RETF(SMP->max_aniso); }
NVN(nvnSamplerGetReductionFilter) { RET(SMP->reduction); }
NVN(nvnSamplerCompare) {
    const NvnSamplerDesc *a = HOST(NvnSamplerDesc, A(0)), *b = HOST(NvnSamplerDesc, A(1));
    RET(a == b || (a && b && memcmp(a, b, offsetof(NvnSamplerDesc, backend)) == 0));
}
NVN(nvnSamplerGetDebugID) { RET(A(0)); }

// ================================================================== descriptor pools
NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];
static NvnDescPool *g_all_desc_pools;
RT_ON_RESET(desc_pools_reset) {
    while (g_all_desc_pools) {
        NvnDescPool *next = g_all_desc_pools->next;
        free(g_all_desc_pools->entries);
        free(g_all_desc_pools);
        g_all_desc_pools = next;
    }
    memset(g_texture_pools, 0, sizeof g_texture_pools);
    memset(g_sampler_pools, 0, sizeof g_sampler_pools);
}

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, NvnDescPool **reg) {
    NvnDescPool *d = calloc(1, sizeof *d);
    d->addr = HOST(NvnPool, pool)->storage + off;
    d->count = count;
    d->entries = calloc(count, entry);
    pthread_mutex_lock(&g_live_lock);
    d->next = g_all_desc_pools;
    g_all_desc_pools = d;
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    pthread_mutex_unlock(&g_live_lock);
    LOG("[nvn] %s pool %p: memory %llx, %d entries of %zu bytes (lr %llx)\n", reg == g_sampler_pools ? "sampler" : "texture", (void *)d,
        (unsigned long long)d->addr, count, entry, 0ull);
    SET_HOST(guest, d);
    G(uint64_t, guest, 8) = pool;
    G(uint64_t, guest, 16) = off;
    G(uint32_t, guest, 24) = (uint32_t)count;
    return d;
}

NVN(nvnTexturePoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), g_texture_pools);
    RET(1);
}
NVN(nvnTexturePoolFinalize) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    if (!d) return;
#ifdef __ANDROID__
    vk_async_drain();
#endif
    NvnTexEntry *entries = d->entries;
    for (int i = 0; i < d->count; i++) {
        NvnTexture *t = entries[i].tex;
        entries[i].tex = NULL;
        if (t) texture_retire(t);
    }
    pthread_mutex_lock(&g_live_lock);
    for (int i = 0; i < 64; i++) if (g_texture_pools[i] == d) g_texture_pools[i] = NULL;
    pthread_mutex_unlock(&g_live_lock);
    // Keep the empty descriptor array until reset: recorded state may still refer to the old pool object.
    SET_HOST(A(0), 0);
}
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnTexturePoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnTexturePoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void register_texture(Cpu *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id < 0 || id >= d->count) return;
    g_tex_stat_reg++;
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    NvnTexture *old = e->tex, *t = HOST(NvnTexture, A(2));
#ifdef __ANDROID__
    // An asynchronous draw must finish reading the previous descriptor before its slot is replaced.
    if (old) vk_async_drain();
#endif
    if (old != t) {
        pthread_mutex_lock(&g_live_lock);
        if (t) t->refs++;
        e->tex = t;
        pthread_mutex_unlock(&g_live_lock);
        if (old) texture_retire(old);
    }
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
    {   // the object's own swizzle, where it shares its texture with an object of another (texture_same_storage)
        const uint8_t *sw = t ? obj_swizzle(A(2)) : NULL;
        if (sw && memcmp(sw, t->swizzle, 4) && !(e->has_view && (e->view.setmask & 8))) {
            if (!e->has_view) memset(&e->view, 0, sizeof e->view);
            e->has_view = true;
            e->view.setmask |= 8;
            memcpy(e->view.swizzle, sw, 4);
        }
    }
    static int watch_id = -2;  // LARECOMP_WATCH_TEXID=<hex id>: its registrations (texture, view)
    if (watch_id == -2) watch_id = getenv("LARECOMP_WATCH_TEXID") ? (int)strtol(getenv("LARECOMP_WATCH_TEXID"), NULL, 16) : -1;
    if (id == watch_id || (watch_id == 0x7fffffff && t && t->width == 320 && t->height == 180 && t->levels == 5))  // (7fffffff: the bloom chain)
        LOG("[watch] texture id %#x <- %p %ux%u levels %u view %d setmask %#x levels %u+%u (lr %llx)\n", id, (void *)t,
            t ? t->width : 0, t ? t->height : 0, t ? t->levels : 0, e->has_view, e->has_view ? e->view.setmask : 0,
            e->view.base_level, e->view.num_levels, (unsigned long long)c->x[30]);
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerDesc), g_sampler_pools);
    RET(1);
}
NVN(nvnSamplerPoolFinalize) { (void)c; }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnSamplerPoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnSamplerPoolGetSize) { RET(G(uint32_t, A(0), 24)); }
// A sampler slot rewritten: the render thread may still have draws queued that read the old one (vulkan/async.c) -
// BOTW recycles slots without a fence wait between (its bloom's mip chain then read a recycled 0..0 LOD clamp: only
// level 0, a blocky glow around the Sheikah lights). Like a texture slot replaced (register_texture): drain first.
static void register_sampler(NvnSamplerDesc *e, const NvnSamplerDesc *n) {
    NvnSamplerDesc v = *n;
    v.backend = NULL;  // (the VkSampler is made per slot, from its fields)
    NvnSamplerDesc o = *e;
    o.backend = NULL;
    if (!memcmp(&o, &v, sizeof v)) return;
#ifdef __ANDROID__
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_SMP_DRAIN") != NULL;
    if (!off) vk_async_drain();
#endif
    *e = v;
}
NVN(nvnSamplerPoolRegisterSampler) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) register_sampler(&((NvnSamplerDesc *)d->entries)[id], HOST(NvnSamplerDesc, A(2)));
    if (getenv("LARECOMP_LOG_SMPREG") && id >= 0 && id < d->count) {  // every registration (diagnosis)
        const NvnSamplerDesc *sd = &((NvnSamplerDesc *)d->entries)[id];
        LOG("[nvn] RegisterSampler %d <- object %llx wrap %u %u %u min %u lod %.1f..%.1f (lr %llx, thread %d)\n", id, (unsigned long long)A(2), sd->wrap[0], sd->wrap[1],
            sd->wrap[2], sd->min_filter, sd->lod_min, sd->lod_max, (unsigned long long)c->x[30], (int)gettid());
    }
    static int watch_id = -2;  // LARECOMP_WATCH_SMPID=<dec id>
    if (watch_id == -2) watch_id = getenv("LARECOMP_WATCH_SMPID") ? atoi(getenv("LARECOMP_WATCH_SMPID")) : -1;
    if (id == watch_id && id < d->count) {
        const NvnSamplerDesc *sd = &((NvnSamplerDesc *)d->entries)[id];
        LOG("[watch] sampler %d <- lod %.2f..%.2f min %u (lr %llx)\n", id, sd->lod_min, sd->lod_max, sd->min_filter, (unsigned long long)c->x[30]);
    }
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) {
        NvnSamplerDesc from = sampler_desc_from_builder(AP(2));  // (the packed builder state)
        register_sampler(&((NvnSamplerDesc *)d->entries)[id], &from);
    }
    if (getenv("LARECOMP_LOG_SMPREG") && id >= 0 && id < d->count)
        LOG("[nvn] RegisterSamplerBuilder %d (lr %llx)\n", id, (unsigned long long)c->x[30]);
}

NvnDescPool *nvn_find_pool(NvnDescPool **reg, uint64_t guest_pool) {
    return guest_pool ? HOST(NvnDescPool, guest_pool) : NULL;
}

// ================================================================== programs
NVN(nvnProgramInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) { free(HOST(NvnProgram, A(0))); SET_HOST(A(0), 0); }
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
#include "shader_control.h"
static void set_shaders(Cpu *c, int stride) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    const uint8_t *data = AP(2);  // NVNshaderData[n] = {u64 data (GPU addr), const void* control}
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(uint64_t *)(data + i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (uint64_t *)(data + i * stride + 8);
        NvnShaderControl info;
        if (!nvn_shader_control(control, 0x880, &info) || code < GUEST_LO || code >= GUEST_HI ||
            info.size > GUEST_HI - code ||
            nvn_control_word((const uint8_t *)(uintptr_t)code, 0) != (info.stage == 5 ? 0x12345679u : 0x12345678u)) {
            LOG("[nvn] invalid shader control/code for program %llx slot %d\n", (unsigned long long)A(0), i);
            memset(p->stages, 0, sizeof p->stages); p->stage_mask = 0; RET(0); return;
        }
        uint32_t stage = info.stage;
        if (stage != 5) {
            static const uint8_t sph_stage[] = {1, 5, 4, 2, 3};
            uint32_t sph0 = nvn_control_word((const uint8_t *)(uintptr_t)code, info.entry);
            if (((sph0 >> 10) & 15) != sph_stage[stage]) { RET(0); return; }
        }
        p->stages[stage].stage = stage;
        p->stages[stage].entry_offset = info.entry;
        p->stages[stage].code_addr = code;
        p->stages[stage].control = control;
        p->stages[stage].const_offset = info.constants;
        p->stages[stage].const_size = info.size - info.constants;
        p->stage_mask |= 1u << stage;
    }
    RET(1);
}

NVN(nvnProgramSetShaders) { set_shaders(c, 16); }    // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, 24); } // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? ((uint64_t *)(uintptr_t)G(uint64_t, A(0), 0x18))[i] : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    memcpy(w->textures, (void *)(uintptr_t)G(uint64_t, A(1), 0x18), w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { RET(WIN->num); }
NVN(nvnWindowGetNumActiveTextures) { RET(WIN->active); }
NVN(nvnWindowSetNumActiveTextures) { WIN->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { RET(WIN->interval); }
// The last frame's duration as sead's frame loop reads it to pick the present interval (tools/patches.py,
// 0x7000c4f4d4: Switch ticks, 19.2 MHz). A fixed frame rate in the menu caps it under 1.9 vsyncs, so the game keeps its
// 30 fps mode (interval 2; BOTW never goes under 2) instead of dropping to 20 - where our GPU's clocks settle low and
// it stayed. Auto (0): the game's own measurement.
int64_t rt_frame_ticks(int64_t ticks) {
    if (!g_settings.fps_mode) return ticks;
    const int64_t cap = 19200000ll * 185 / 100 / 60;  // (1.85 vsyncs)
    return ticks > cap ? cap : ticks;
}

#ifdef __ANDROID__
int g_present_interval = 1;  // the game's latest (1: 60 fps, 2: 30 fps): the frame pacer (vulkan/exec.c)
NVN(nvnWindowSetPresentInterval) {
    static int force = -1;  // (LARECOMP_FORCE_INTERVAL=<n>, diagnosis: the game's choice replaced - slow motion if it doesn't fit)
    if (force < 0) force = getenv("LARECOMP_FORCE_INTERVAL") ? atoi(getenv("LARECOMP_FORCE_INTERVAL")) : 0;
    if (force > 0) c->x[1] = (uint64_t)force;
    // BOTW 1.8.2's frame loop (lr 0x7001012a34, x19 = the framework; [fw+0x234] the highest interval it may pick):
    // never 3 while the runtime sets the game's clock (runtime/hle/framepace.c), as SavageWindRecomp does
    if (framepace_on() && c->x[30] == GUEST_LO + 0x1012a34ull) {
        uint32_t *max_iv = (uint32_t *)(uintptr_t)(c->x[19] + 0x234);
        if (*max_iv > 2) *max_iv = 2;
    }
    if ((int)A(1) != g_present_interval)
        LOG("[pace] the game sets its present interval to %d (lr %llx, x19 %llx [+0x240] %lld)\n", (int)A(1),
            (unsigned long long)c->x[30], (unsigned long long)c->x[19],
            c->x[19] > 0x100000000ull ? (long long)*(const int64_t *)(uintptr_t)(c->x[19] + 0x240) : 0ll);
    WIN->interval = g_present_interval = (int)A(1);
}
#else
NVN(nvnWindowSetPresentInterval) { WIN->interval = (int)A(1); }
#endif
NVN(nvnWindowSetCrop) { int *cr = WIN->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4); }
NVN(nvnWindowGetCrop) { memcpy(AP(1), WIN->crop, 16); }
NVN(nvnWindowAcquireTexture) {
    NvnWindow *w = WIN;
    if (A(1)) sync_of(A(1))->signaled = 1;
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
NVN(nvnQueuePresentTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    int i = (int)A(2);
    {   // Frame limiter: a present waits until N vsyncs have gone by since the previous one, as NVN does on the Switch -
        // N is the larger of the window's interval (BOTW asks for 2: its world steps 1/30 s a frame) and the Video
        // tab's cap (Frame rate 30: 2, 60: 1; Auto: 2 for now). Without it the menus ran at ~200 fps and the world
        // ran fast whenever a frame came in under 33 ms. LARECOMP_PRESENT_WAIT=0: off.
        static int on = -1;
        static uint64_t last_vs;
        if (on < 0) on = !getenv("LARECOMP_PRESENT_WAIT") || atoi(getenv("LARECOMP_PRESENT_WAIT"));
        extern volatile uint64_t g_vsync_count;
        extern __thread uint64_t g_blocked_ns;  // (kernel.c: the pacer counts this as idle, not frame work)
        int cap = g_settings.fps_mode == 2 ? 1 : 2;
        int n = w->interval > cap ? w->interval : cap;
        if (on && framepace_on()) {
            // BOTW 1.8.2 (runtime/hle/framepace.c): a steady grid at the chosen frame rate, whatever the interval;
            // after a long frame the grid starts again. framepace.c sets the game's clock from when it went out.
            static uint64_t last;
            uint64_t period = framepace_period_ns(), target = last + period, t0 = plat_now_ns();
            if (last && t0 < target) plat_sleep_until_ns(target);
            uint64_t t1 = plat_now_ns();
            g_blocked_ns += t1 - t0;
            last = last && t1 < target + period ? target : t1;
            framepace_presented(last, last == target);
        } else if (on) {
            uint64_t want = last_vs + (uint64_t)n, t0 = plat_now_ns();
            while (__atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED) < want) usleep(500);
            g_blocked_ns += plat_now_ns() - t0;
            last_vs = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
        }
    }
    if (i >= 0 && i < w->num) backend_present(HOST(NvnTexture, w->textures[i]));
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1);
    uint32_t m = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> ((int)A(1) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bit = (uint8_t)(1u << A(1));
    G(uint8_t, A(0), 0) = A(2) ? (m | bit) : (m & ~bit);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> A(1)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (d[4] & 0xF0) | (A(2) & 0xF);
    if (face & 2) d[4] = (d[4] & 0x0F) | (uint8_t)((A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int64_t *)AP(2) = v >> 13;
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
