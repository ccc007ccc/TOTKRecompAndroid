#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/nvn.h"
#include "../runtime/nvn/submit_snapshot.h"

static _Alignas(16) uint8_t guest[32768];
static _Alignas(8) uint8_t list[8192];
static size_t used;
static bool pool_live = true;
static uint64_t address(size_t offset) { return (uint64_t)(uintptr_t)(guest + offset); }
static bool read_pool(uint64_t addr, uint64_t size, void *dst, uint64_t *lo, uint64_t *hi) {
    if (!pool_live || !size || size > UINT64_MAX - addr) return false;
    uint64_t pools[2][2] = {{address(0), address(8192)}, {address(8192), address(sizeof guest)}};
    for (int i = 0; i < 2; i++) if (addr >= pools[i][0] && addr < pools[i][1] && size <= pools[i][1] - addr) {
        *lo = pools[i][0]; *hi = pools[i][1];
        if (dst) memcpy(dst, (const void *)(uintptr_t)addr, (size_t)size);
        return true;
    }
    return false;
}
static CmdRec *record(unsigned op, size_t extra) {
    size_t size = (sizeof(CmdRec) + extra + 7) & ~(size_t)7;
    assert(used + size <= sizeof list);
    CmdRec *r = (CmdRec *)(list + used);
    memset(r, 0, size); r->op = op; r->size = (uint32_t)size;
    used += size;
    return r;
}
static void binding(unsigned op, unsigned slot, uint64_t addr, uint64_t size) {
    CmdRec *r = record(op, 16);
    r->x[0] = 0; r->x[1] = slot; r->x[2] = 1;
    uint64_t range[2] = {addr, size}; memcpy(r->extra, range, sizeof range);
}
static SubmitSnapshot *capture(uint64_t retired, bool *sync) {
    record(CMD_END, 0);
    SubmitSnapshot *s = submit_snapshot_capture(list, used, retired, read_pool, sync);
    used -= sizeof(CmdRec);
    return s;
}
static void reset(void) {
    assert(submit_snapshot_inflight_bytes() == 0);
    submit_snapshot_end(); submit_snapshot_reset(); used = 0; pool_live = true;
    memset(guest, 17, sizeof guest);
}
static void done(SubmitSnapshot *s) { submit_snapshot_end(); submit_snapshot_free(s); }

// The producer overwrites the same guest constants while the replay checks older requests. Three requests are
// enough to expose reading live bytes or freeing a payload before the renderer has copied it.
typedef struct { SubmitSnapshot *s; uint64_t expected; } Request;
static Request queue[3];
static int head, tail, queued;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t more = PTHREAD_COND_INITIALIZER, less = PTHREAD_COND_INITIALIZER;
static void *replay(void *arg) {
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&lock);
        while (!queued) pthread_cond_wait(&more, &lock);
        Request request = queue[head++ % 3]; queued--;
        pthread_cond_signal(&less); pthread_mutex_unlock(&lock);
        submit_snapshot_begin(request.s);
        const uint64_t *data = submit_snapshot_data(address(64), 16);
        assert(data && data[0] == request.expected && data[1] == ~request.expected);
        done(request.s);
    }
    return NULL;
}

int main(int argc, char **argv) {
    bool sync = false;
    if (argc > 1 && !strcmp(argv[1], "--disabled")) {
        assert(!submit_snapshot_enabled());
        binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(64), 16);
        assert(!capture(0, &sync) && !sync && submit_snapshot_inflight_bytes() == 0);
        puts("Submit snapshots disabled: baseline path preserved"); return 0;
    }
    assert(submit_snapshot_enabled());
    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(128), 64);
    binding(CMD_BIND_STORAGE_BUFFERS, 0, address(512), 64);
    CmdRec *copy = record(CMD_COPY_BUFFER_TO_BUFFER, 0);
    copy->x[0] = address(2048); copy->x[1] = address(4096); copy->x[2] = 64;
    SubmitSnapshot *a = capture(0, &sync); assert(a && !sync);
    SubmitSnapshot *b = capture(0, &sync); assert(b && !sync);
    memset(guest, 42, sizeof guest);
    submit_snapshot_begin(a);
    assert(*(const uint8_t *)submit_snapshot_data(address(128), 64) == 17);
    assert(*(const uint8_t *)submit_snapshot_data(address(512), 64) == 17);
    assert(*(const uint8_t *)submit_snapshot_data(address(2048), 64) == 17);
    uint8_t margin[96]; submit_snapshot_copy(address(112), sizeof margin, margin);
    assert(margin[0] == 42 && margin[16] == 17 && margin[79] == 17 && margin[80] == 42);
    submit_snapshot_write(address(144), 4, 5);  // inline/copy/storage aliases invalidate the whole captured region
    assert(!submit_snapshot_data(address(128), 64));
    assert(*(const uint8_t *)submit_snapshot_data(address(512), 64) == 17);
    done(a);
    submit_snapshot_begin(b);  // its frozen capture boundary predates the GPU write, even if the GPU is now done
    assert(!submit_snapshot_data(address(128), 64));
    assert(*(const uint8_t *)submit_snapshot_data(address(512), 64) == 17);
    done(b);
    used = 0;  // no new binds: the submission inherits the prior UBO / SSBO state
    SubmitSnapshot *c = capture(5, &sync); assert(c && !sync);
    submit_snapshot_begin(c);
    assert(*(const uint8_t *)submit_snapshot_data(address(128), 64) == 42);
    assert(*(const uint8_t *)submit_snapshot_data(address(512), 64) == 42);
    assert(!submit_snapshot_data(address(2048), 64));  // a prior copy source is not an inherited binding
    done(c);

    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(256), 64);
    copy = record(CMD_COPY_BUFFER_TO_BUFFER, 0);
    copy->x[0] = address(255); copy->x[1] = address(4096); copy->x[2] = 80;
    a = capture(0, &sync); assert(a && !sync);
    pool_live = false;  // releasing an allocation cannot invalidate already captured CPU bytes
    submit_snapshot_begin(a);
    const void *aligned = submit_snapshot_data(address(256), 64);
    assert(aligned && ((uintptr_t)aligned & 15) == (address(256) & 15));
    assert(*(const uint8_t *)aligned == 17);
    done(a);

    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(64), 5000);
    assert(!capture(0, &sync) && sync && !submit_snapshot_inflight_bytes());  // per-request cap, no reservation underflow
    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(64), 3000);
    a = capture(0, &sync); assert(a && !sync);
    b = capture(0, &sync); assert(b && !sync);
    assert(!capture(0, &sync) && sync && submit_snapshot_inflight_bytes() == 6000);  // in-flight budget
    done(a); done(b);
    c = capture(0, &sync); assert(c && !sync); done(c);  // budget reused after replay

    reset();
    for (unsigned i = 0; i < 40; i++) binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(64 + 32 * i), 16);
    assert(!capture(0, &sync) && sync && !submit_snapshot_inflight_bytes());
    used = 0; c = capture(0, &sync); assert(c && !sync);
    submit_snapshot_begin(c);
    assert(submit_snapshot_data(address(64 + 32 * 39), 16));  // fallback still updates the full binding shadow
    done(c);

    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(8000), 256);  // cannot bridge separate pool allocations
    a = capture(0, &sync); assert(a && sync && !submit_snapshot_inflight_bytes()); done(a);
    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, UINT64_MAX - 8, 16);
    assert(!capture(0, &sync) && sync && !submit_snapshot_inflight_bytes());
    reset();
    CmdRec *bad = record(CMD_BIND_UNIFORM_BUFFERS, 0); bad->x[2] = 1;
    assert(!capture(0, &sync) && sync && !submit_snapshot_inflight_bytes());

    reset();
    binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(4096), 64);
    a = capture(0, &sync); assert(a && !sync);
    for (unsigned i = 0; i < 10; i++) submit_snapshot_write(address(64 + 16 * i), 8, 99);
    submit_snapshot_begin(a);
    assert(!submit_snapshot_data(address(4096), 64));  // unrecorded GPU write: drop all captures, never silently accept
    done(a);
    c = capture(99, &sync); assert(c && !sync);
    submit_snapshot_begin(c); assert(submit_snapshot_data(address(4096), 64)); done(c);

    reset();
    pthread_t consumer; assert(!pthread_create(&consumer, NULL, replay, NULL));
    for (uint64_t i = 1; i <= 10000; i++) {
        used = 0;
        uint64_t words[2] = {i, ~i}; memcpy(guest + 64, words, sizeof words);
        binding(CMD_BIND_UNIFORM_BUFFERS, 0, address(64), 16);
        SubmitSnapshot *s = capture(0, &sync); assert(s && !sync);
        pthread_mutex_lock(&lock);
        while (queued == 3) pthread_cond_wait(&less, &lock);
        queue[tail++ % 3] = (Request){s, i}; queued++;
        pthread_cond_signal(&more); pthread_mutex_unlock(&lock);
    }
    assert(!pthread_join(consumer, NULL));
    assert(submit_snapshot_inflight_bytes() == 0);
    puts("Submit snapshots: UBO/SSBO/copy, inheritance, GPU ordering/alias, bounds/budgets and 10000 producer/replay requests passed");
    return 0;
}
