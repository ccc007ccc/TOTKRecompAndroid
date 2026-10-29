// Submit-time UBO / SSBO / CopyBuffer source bytes. Source mechanism: MacNativeGames@ff63a162 (MIT; license in hle/).
// Payloads are immutable until their request finishes. GPU writes invalidate overlapping captures from that point,
// and a later capture is rejected at replay if an older GPU write was not retired before the capture began.
#ifdef __ANDROID__
#include "../diagnostics_build.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "submit_snapshot.h"

#ifndef SUBMIT_SNAPSHOT_BYTES
#define SUBMIT_SNAPSHOT_BYTES (16u << 20)
#endif
#ifndef SUBMIT_SNAPSHOT_BUDGET
#define SUBMIT_SNAPSHOT_BUDGET (64u << 20)
#endif
#ifndef SUBMIT_SNAPSHOT_RANGES
#define SUBMIT_SNAPSHOT_RANGES 32768u
#endif
#ifndef SUBMIT_SNAPSHOT_WRITES
#define SUBMIT_SNAPSHOT_WRITES 65536u
#endif
typedef struct { uint64_t lo, hi; size_t off; bool valid; } CopyRange;
typedef struct { uint64_t addr, size; } BoundRange;
typedef struct { uint64_t lo, hi, seq; } WriteRange;
struct SubmitSnapshot { uint64_t retired; CopyRange *ranges; size_t n, cap, size; uint8_t *bytes; bool reserved; };
static BoundRange ubos[6][18], sbos[6][16];  // producer-side inherited bindings
static WriteRange writes[SUBMIT_SNAPSHOT_WRITES];  // render thread only
static size_t nwrites;
static uint64_t overflow_seq;
static _Thread_local SubmitSnapshot *current;
static pthread_mutex_t budget_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t inflight;
static _Atomic int enabled = -1;
static _Atomic uint64_t captures, copied, fallbacks, dropped;

bool submit_snapshot_enabled(void) {
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        const char *e = getenv("LARECOMP_VK_SUBMIT_SNAPSHOT");
        value = e && atoi(e) != 0;  // experimental until gameplay and performance checks justify enabling by default
        int expected = -1;
        if (atomic_compare_exchange_strong(&enabled, &expected, value))
            LOG("[submit-snapshot] %s; per-request cap %u bytes, in-flight cap %u bytes\n", value ? "on" : "off",
                SUBMIT_SNAPSHOT_BYTES, SUBMIT_SNAPSHOT_BUDGET);
    }
    return value != 0;
}

static bool append(SubmitSnapshot *s, uint64_t addr, uint64_t size) {
    if (!addr || !size) return true;
    if (size > UINT64_MAX - addr || s->n == SUBMIT_SNAPSHOT_RANGES) return false;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        if (cap > SUBMIT_SNAPSHOT_RANGES) cap = SUBMIT_SNAPSHOT_RANGES;
        CopyRange *ranges = realloc(s->ranges, cap * sizeof *ranges);
        if (!ranges) return false;
        s->ranges = ranges; s->cap = cap;
    }
    s->ranges[s->n++] = (CopyRange){.lo = addr, .hi = addr + size, .valid = true};
    return true;
}

static int by_start(const void *a, const void *b) {
    const CopyRange *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static void discard(SubmitSnapshot *s, bool *sync) {
    *sync = true;
    atomic_fetch_add_explicit(&fallbacks, 1, memory_order_relaxed);
    submit_snapshot_free(s);
}

SubmitSnapshot *submit_snapshot_capture(const void *list, size_t bytes, uint64_t retired, SubmitRead read, bool *sync) {
    *sync = false;
    if (!submit_snapshot_enabled()) return NULL;
    SubmitSnapshot *s = calloc(1, sizeof *s);
    if (!s) { *sync = true; return NULL; }
    s->retired = retired;
    bool ok = true;
    for (int stage = 0; stage < 6; stage++) {
        for (int i = 0; i < 18; i++) ok &= append(s, ubos[stage][i].addr, ubos[stage][i].size);
        for (int i = 0; i < 16; i++) ok &= append(s, sbos[stage][i].addr, sbos[stage][i].size);
    }
    for (size_t offset = 0; offset < bytes;) {
        if (bytes - offset < sizeof(CmdRec)) { ok = false; break; }
        const CmdRec *r = (const CmdRec *)((const uint8_t *)list + offset);
        if (r->size < sizeof *r || (r->size & 7) || r->size > bytes - offset || r->op >= CMD_COUNT) { ok = false; break; }
        if (r->op == CMD_END) break;
        if (r->op == CMD_CALL || r->op == CMD_JUMP) { ok = false; break; }  // capture only a complete flattened list
        if (r->op == CMD_BIND_UNIFORM_BUFFERS || r->op == CMD_BIND_STORAGE_BUFFERS) {
            uint64_t max = r->op == CMD_BIND_UNIFORM_BUFFERS ? 18 : 16;
            if (r->x[0] < 6 && r->x[1] < max) {
                uint64_t count = r->x[2] < max - r->x[1] ? r->x[2] : max - r->x[1];
                if (count > (r->size - sizeof *r) / sizeof(BoundRange)) { ok = false; break; }
                BoundRange *bound = r->op == CMD_BIND_UNIFORM_BUFFERS ? ubos[r->x[0]] : sbos[r->x[0]];
                for (uint64_t i = 0; i < count; i++) {
                    BoundRange value;
                    memcpy(&value, r->extra + i * sizeof value, sizeof value);
                    bound[r->x[1] + i] = value;
                    ok &= append(s, value.addr, value.size);
                }
            }
        } else if (r->op == CMD_COPY_BUFFER_TO_BUFFER) {
            ok &= append(s, r->x[0], r->x[2]);
        }
        offset += r->size;
    }
    // Do not preserve a partial snapshot after malformed recordings or resource exhaustion. Translation must finish
    // inside the submit call in this rare case, before the caller can recycle any omitted CPU inputs.
    if (!ok) { discard(s, sync); return NULL; }
    if (s->n) qsort(s->ranges, s->n, sizeof *s->ranges, by_start);
    size_t n = 0;
    uint64_t last_lo = 1, last_hi = 0;
    for (size_t i = 0; i < s->n; i++) {
        CopyRange value = s->ranges[i];
        uint64_t pool_lo, pool_hi;
        if (n && value.lo <= s->ranges[n - 1].hi && value.hi <= last_hi) {
            if (value.hi > s->ranges[n - 1].hi) s->ranges[n - 1].hi = value.hi;
            continue;
        }
        if (!(value.lo >= last_lo && value.hi <= last_hi)) {
            if (!read(value.lo, value.hi - value.lo, NULL, &pool_lo, &pool_hi)) {
                // Shader constants/other non-pool addresses retain the existing fence-governed path. Capture never
                // dereferences an unregistered range or bridges two different allocations.
                *sync = true;
                continue;
            }
            last_lo = pool_lo; last_hi = pool_hi;
        }
        s->ranges[n++] = value;
    }
    s->n = n;
    for (size_t i = 0; i < n; i++) {
        uint64_t size = s->ranges[i].hi - s->ranges[i].lo;
        if (size > SUBMIT_SNAPSHOT_BYTES - s->size) { discard(s, sync); return NULL; }
        // Preserve source alignment for existing typed loads (bindless handles / camera constants).
        size_t offset = ((s->size + 15) & ~(size_t)15) + (size_t)(s->ranges[i].lo & 15);
        if (offset > SUBMIT_SNAPSHOT_BYTES || size > SUBMIT_SNAPSHOT_BYTES - offset) { discard(s, sync); return NULL; }
        s->ranges[i].off = offset;
        s->size = offset + (size_t)size;
    }
    if (s->size) {
        pthread_mutex_lock(&budget_lock);
        if (s->size > SUBMIT_SNAPSHOT_BUDGET - inflight) {
            pthread_mutex_unlock(&budget_lock);
            discard(s, sync); return NULL;
        }
        inflight += s->size;
        s->reserved = true;
        pthread_mutex_unlock(&budget_lock);
        s->bytes = malloc(s->size);
        if (!s->bytes) { discard(s, sync); return NULL; }
        for (size_t i = 0; i < n; i++) {
            CopyRange *range = &s->ranges[i];
            uint64_t lo, hi;
            if (!read(range->lo, range->hi - range->lo, s->bytes + range->off, &lo, &hi)) {
                range->valid = false; *sync = true;
            }
        }
    }
    if (*sync) atomic_fetch_add_explicit(&fallbacks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&copied, s->size, memory_order_relaxed);
    uint64_t count = atomic_fetch_add_explicit(&captures, 1, memory_order_relaxed) + 1;
    static int log = -1;  // producer lock covers initialization
    if (log < 0) log = diag_getenv("LARECOMP_LOG_SUBMIT_SNAPSHOT") != NULL;
    if (log && count % 120 == 0)
        LOG("[submit-snapshot] captures %llu, bytes %llu, sync fallbacks %llu, dropped ranges %llu\n",
            (unsigned long long)count, (unsigned long long)atomic_load(&copied),
            (unsigned long long)atomic_load(&fallbacks), (unsigned long long)atomic_load(&dropped));
    return s;
}

static bool overlaps(uint64_t lo, uint64_t hi, const CopyRange *range) { return lo < range->hi && range->lo < hi; }

void submit_snapshot_begin(SubmitSnapshot *s) {
    current = s;
    if (!s) return;
    // Retire using the frozen CAPTURE boundary, never the current GPU counter: an earlier write completing while a
    // request waited in the queue must still invalidate bytes copied before that write completed.
    size_t n = 0;
    for (size_t i = 0; i < nwrites; i++) if (writes[i].seq > s->retired) {
        writes[n++] = writes[i];
        for (size_t j = 0; j < s->n; j++)
            if (s->ranges[j].valid && overlaps(writes[i].lo, writes[i].hi, &s->ranges[j])) {
                s->ranges[j].valid = false;
                atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
            }
    }
    nwrites = n;
    if (overflow_seq > s->retired) for (size_t i = 0; i < s->n; i++) s->ranges[i].valid = false;
    else overflow_seq = 0;
}

void submit_snapshot_end(void) { current = NULL; }

const void *submit_snapshot_data(uint64_t addr, uint64_t size) {
    SubmitSnapshot *s = current;
    if (!s || !size || size > UINT64_MAX - addr) return NULL;
    size_t lo = 0, hi = s->n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (s->ranges[mid].hi <= addr) lo = mid + 1; else hi = mid; }
    if (lo == s->n) return NULL;
    for (; lo < s->n && s->ranges[lo].lo <= addr; lo++) {
        const CopyRange *range = &s->ranges[lo];
        if (range->valid && addr + size <= range->hi) return s->bytes + range->off + (addr - range->lo);
    }
    return NULL;
}

void submit_snapshot_copy(uint64_t addr, uint64_t size, void *dst) {
    if (!size) return;
    const void *data = submit_snapshot_data(addr, size);
    if (data) { memcpy(dst, data, (size_t)size); return; }
    memcpy(dst, (const void *)(uintptr_t)addr, (size_t)size);
    // Arena copies include alignment/margin bytes outside declared bindings. Preserve any contained captured bytes
    // while leaving that padding on its existing path. Invalidated ranges are never overlaid.
    if (!current || size > UINT64_MAX - addr) return;
    for (size_t i = 0; i < current->n && current->ranges[i].lo < addr + size; i++) {
        const CopyRange *range = &current->ranges[i];
        if (!range->valid || range->hi <= addr) continue;
        uint64_t lo = range->lo > addr ? range->lo : addr, hi = range->hi < addr + size ? range->hi : addr + size;
        memcpy((uint8_t *)dst + (lo - addr), current->bytes + range->off + (lo - range->lo), (size_t)(hi - lo));
    }
}

void submit_snapshot_write(uint64_t addr, uint64_t size, uint64_t seq) {
    if (!size || size > UINT64_MAX - addr || !submit_snapshot_enabled()) return;
    uint64_t hi = addr + size;
    if (current) for (size_t i = 0; i < current->n; i++)
        if (current->ranges[i].valid && overlaps(addr, hi, &current->ranges[i])) {
            current->ranges[i].valid = false;
            atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        }
    // Repeated stores to the same buffer in one GPU submission need only one history entry.
    for (size_t i = nwrites, end = i > 8 ? i - 8 : 0; i > end;)
        if (writes[--i].lo == addr && writes[i].hi == hi && writes[i].seq == seq) return;
    if (nwrites < SUBMIT_SNAPSHOT_WRITES) writes[nwrites++] = (WriteRange){addr, hi, seq};
    else if (seq > overflow_seq) overflow_seq = seq;  // history exhaustion drops all later captures conservatively
}

void submit_snapshot_free(SubmitSnapshot *s) {
    if (!s) return;
    if (s->reserved) {
        pthread_mutex_lock(&budget_lock);
        inflight -= s->size;
        pthread_mutex_unlock(&budget_lock);
    }
    free(s->bytes); free(s->ranges); free(s);
}

uint64_t submit_snapshot_inflight_bytes(void) {
    pthread_mutex_lock(&budget_lock);
    uint64_t size = inflight;
    pthread_mutex_unlock(&budget_lock);
    return size;
}

void submit_snapshot_reset(void) {
    current = NULL; nwrites = 0; overflow_seq = 0;
    memset(ubos, 0, sizeof ubos); memset(sbos, 0, sizeof sbos);
}
#else
typedef int submit_snapshot_unused;
#endif
