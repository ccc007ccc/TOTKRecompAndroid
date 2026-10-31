// A small Horizon kernel: handles, threads, synchronization, memory, and the SVC dispatcher.
#include "diagnostics_build.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if TOTK_DIAGNOSTICS && defined(__linux__)
#include <sys/syscall.h>
#endif

#include "hle.h"
#include "kernel.h"

// ------------------------------------------------------------------ results
#undef R_OK
#define R_OK 0
#define R_TIMEOUT 0xEA01
#define R_CANCELLED 0xEC01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_STATE 0xFA01
#define R_INVALID_ADDRESS 0xCC01
#define R_NOT_FOUND 0xF201
#define R_INVALID_ENUM 0xF001
#define R_OUT_OF_RESOURCE 0xD001
#define R_INVALID_CORE 0xE801
#define R_INVALID_COMBINATION 0xF001

#define PSEUDO_THREAD 0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u
#define WAITERS_BIT 0x40000000u

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static __thread KThread *t_cur;
// Priority also participates in the core-0 queue under c0_m. Keep the existing
// field/layout, with atomic access across the two lock domains.
static int thread_priority(const KThread *t) { return __atomic_load_n(&t->prio, __ATOMIC_RELAXED); }

// ------------------------------------------------------------------ handle table
#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];

static uint32_t handle_alloc(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            return 0x100 + i;
        }
    }
    FATAL("out of handles\n");
}

KObject *handle_get(uint32_t h) {
    if (h == PSEUDO_THREAD) return &t_cur->obj;
    if (h < 0x101 || h >= 0x100 + MAX_HANDLES) return NULL;
    return handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    pthread_mutex_lock(&g_klock);
    uint32_t h = handle_alloc(o);
    pthread_mutex_unlock(&g_klock);
    return h;
}

static void handle_close(uint32_t h) {
    KObject *o = handle_get(h);
    if (!o || h == PSEUDO_THREAD) return;
    handles[h - 0x100] = NULL;
    o->refs--;
}

// Threads blocked in WaitSynchronization; object signals wake only these (caller holds g_klock).
static KThread *g_syncwaiters;

static void wake(KThread *t) { pthread_cond_signal(&t->cond); }

void kernel_signal(KObject *o) {
    // caller holds g_klock
    o->signaled = true;
    for (KThread *t = g_syncwaiters; t; t = t->snext) wake(t);
}

// ------------------------------------------------------------------ time

uint64_t rt_ticks(void) {
    // 19.2 MHz system tick
    uint64_t ns = plat_now_ns();
    static double scale = -1;
    if (scale < 0) scale = getenv("LARECOMP_TICK_SCALE") ? atof(getenv("LARECOMP_TICK_SCALE")) : 1.0;
    if (scale != 1.0) ns = (uint64_t)(ns * scale);
    return (uint64_t)((__uint128_t)ns * 192 / 10000);
}

static void deadline(struct timespec *ts, int64_t timeout_ns) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ns / 1000000000;
    ts->tv_nsec += timeout_ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

// Sleep on the current thread's condition. Returns false on timeout. Caller holds g_klock.
static pthread_cond_t g_nothread_cond = PTHREAD_COND_INITIALIZER;
// Time this thread spent blocked in the kernel (waits, sleeps): idle, not work. Frame pacing (exec.c vk_pace_frame)
// takes the presenting thread's: a frame's busy time is its period minus this.
__thread uint64_t g_blocked_ns;

#if TOTK_DIAGNOSTICS
enum { WAIT_PROBE_CORE0=0x80, WAIT_PROBE_HOST, WAIT_PROBE_COUNT };
static __thread struct {
    uint64_t ns[WAIT_PROBE_COUNT], calls[WAIT_PROBE_COUNT], max_ns[WAIT_PROBE_COUNT];
    uint64_t lr[WAIT_PROBE_COUNT], args[WAIT_PROBE_COUNT][4], since, frame, cpu_ns;
    uint64_t address_waker, address_waker_lr;
    uint64_t service_ns[0x80], service_calls[0x80], service_body_ns[0x80];
    uint64_t yields[3];
} wait_probe;
static _Atomic uint64_t wait_probe_presenter, wait_probe_signaler, wait_probe_signal_lr, wait_probe_frame, wait_probe_worker;
// Accessed only under g_klock. An id check makes modulo collisions a missing
// diagnostic, never a false attribution to an unrelated thread.
static struct {uint64_t target,thread,lr;} wait_probe_wakers[256];
static __thread uint64_t wait_probe_trace_addr[4], wait_probe_trace_pending;
static __thread unsigned wait_probe_trace_count;
static bool wait_probe_on(void) {
    static _Atomic int enabled=-1;
    int value=atomic_load_explicit(&enabled,memory_order_relaxed);
    if(value<0){const char *e=diag_getenv("LARECOMP_LOG_KERNEL_WAITS");value=e&&atoi(e)!=0;atomic_store_explicit(&enabled,value,memory_order_relaxed);}
    return value!=0;
}
// Keep only the longest condition wait of each SVC. Capture its wake identity
// under g_klock, then print after svc_entry and the core-0 handoff have returned.
static __thread struct {
    uint64_t start, end, frame, thread, lr, address, waker, waker_lr;
    unsigned svc, reports;
} wait_probe_long;
static uint64_t wait_probe_long_first(void) {
    static _Atomic uint64_t configured=UINT64_MAX;
    uint64_t value=atomic_load_explicit(&configured,memory_order_relaxed);
    if(value==UINT64_MAX) {
        const char *e=diag_getenv("LARECOMP_KERNEL_WAIT_TRACE_FRAME");
        value=e?strtoull(e,NULL,10):UINT64_MAX-1;
        atomic_store_explicit(&configured,value,memory_order_relaxed);
    }
    return value;
}
static void wait_probe_long_record(uint64_t start,uint64_t end) {
    if(!wait_probe_on() || !t_cur || !t_cur->svc_now || end<start || end-start<50000000ull)return;
    uint64_t frame=atomic_load_explicit(&wait_probe_frame,memory_order_relaxed);
    if(frame<wait_probe_long_first() || wait_probe_long.reports>=128 ||
       end-start<=wait_probe_long.end-wait_probe_long.start)return;
    wait_probe_long.start=start;wait_probe_long.end=end;wait_probe_long.frame=frame;
    wait_probe_long.thread=t_cur->id;wait_probe_long.svc=t_cur->svc_now-1;
    wait_probe_long.lr=t_cur->svc_lr;wait_probe_long.address=t_cur->svc_args[0];
    unsigned slot=(unsigned)(t_cur->id%256);
    bool match=t_cur->svc_now==0x35 && wait_probe_wakers[slot].target==t_cur->id;
    wait_probe_long.waker=match?wait_probe_wakers[slot].thread:0;
    wait_probe_long.waker_lr=match?wait_probe_wakers[slot].lr:0;
}
static void wait_probe_long_flush(void) {
    if(!wait_probe_long.end)return;
    LOG("[kernel-long-wait] frame %llu thread %llu svc-%02x ns %llu..%llu wall %.3f ms lr %llx arg0 %llx waker %llu lr %llx\n",
        (unsigned long long)wait_probe_long.frame,(unsigned long long)wait_probe_long.thread,wait_probe_long.svc,
        (unsigned long long)wait_probe_long.start,(unsigned long long)wait_probe_long.end,
        (wait_probe_long.end-wait_probe_long.start)/1e6,(unsigned long long)wait_probe_long.lr,
        (unsigned long long)wait_probe_long.address,(unsigned long long)wait_probe_long.waker,
        (unsigned long long)wait_probe_long.waker_lr);
    ++wait_probe_long.reports;wait_probe_long.start=wait_probe_long.end=0;
}
static void wait_probe_add(unsigned kind,uint64_t ns) {
    if(!wait_probe_on() || kind>=WAIT_PROBE_COUNT)return;
    wait_probe.ns[kind]+=ns;wait_probe.calls[kind]++;
    if(ns>wait_probe.max_ns[kind]) {
        wait_probe.max_ns[kind]=ns;
        if(t_cur && kind<0x80) {
            wait_probe.lr[kind]=t_cur->svc_lr;
            for(unsigned i=0;i<4;++i)wait_probe.args[kind][i]=t_cur->svc_args[i];
            if(kind==0x34) { // kwait returned with g_klock held
                unsigned slot=(unsigned)(t_cur->id%256);
                bool match=wait_probe_wakers[slot].target==t_cur->id;
                wait_probe.address_waker=match?wait_probe_wakers[slot].thread:0;
                wait_probe.address_waker_lr=match?wait_probe_wakers[slot].lr:0;
                if(t_cur->id==atomic_load_explicit(&wait_probe_signaler,memory_order_relaxed))
                    atomic_store_explicit(&wait_probe_worker,wait_probe.address_waker,memory_order_relaxed);
            }
        }
    }
    if(kind==0x34 && ns>=30000000ull && t_cur &&
       t_cur->id==atomic_load_explicit(&wait_probe_signaler,memory_order_relaxed))
        wait_probe_trace_pending=t_cur->svc_args[0];
}
static void wait_probe_report(uint64_t frame) {
    uint64_t now=plat_now_ns();
    if(wait_probe.since && now-wait_probe.since<2000000000ull)return;
    struct timespec cpu_time;
    uint64_t cpu_ns=clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_time)==0 ?
        (uint64_t)cpu_time.tv_sec*1000000000ull+(uint64_t)cpu_time.tv_nsec : 0;
    if(!wait_probe.since){memset(&wait_probe,0,sizeof wait_probe);wait_probe.since=now;wait_probe.frame=frame;wait_probe.cpu_ns=cpu_ns;return;}
    uint64_t frames=frame-wait_probe.frame;
    long host_tid=0;
#if defined(__linux__)
    host_tid=syscall(SYS_gettid);
#endif
    double cpu_ms=cpu_ns && wait_probe.cpu_ns && cpu_ns>=wait_probe.cpu_ns ? (cpu_ns-wait_probe.cpu_ns)/1e6 : -1;
    LOG("[kernel-work] frame %llu frames %llu thread %llu host %ld wall %.3f cpu %.3f ms per-frame %.3f\n",
        (unsigned long long)frame,(unsigned long long)frames,(unsigned long long)(t_cur?t_cur->id:0),host_tid,
        (now-wait_probe.since)/1e6,cpu_ms,frames && cpu_ms>=0?cpu_ms/frames:-1);
    for(unsigned i=0;i<0x80;++i)if(wait_probe.service_calls[i])
        LOG("[kernel-service] frame %llu frames %llu thread %llu svc-%02x calls %llu wall %.3f per-frame %.3f body %.3f ms\n",
            (unsigned long long)frame,(unsigned long long)frames,(unsigned long long)(t_cur?t_cur->id:0),i,
            (unsigned long long)wait_probe.service_calls[i],wait_probe.service_ns[i]/1e6,frames?wait_probe.service_ns[i]/1e6/frames:0,
            frames?wait_probe.service_body_ns[i]/1e6/frames:0);
    if(wait_probe.service_calls[0x0b])LOG("[kernel-yields] thread %llu negative %llu zero %llu positive %llu\n",
        (unsigned long long)(t_cur?t_cur->id:0),(unsigned long long)wait_probe.yields[0],
        (unsigned long long)wait_probe.yields[1],(unsigned long long)wait_probe.yields[2]);
    for(unsigned i=0;i<WAIT_PROBE_COUNT;++i)if(wait_probe.calls[i]) {
        LOG("[kernel-waits] frame %llu frames %llu thread %llu %s%02x calls %llu total %.3f per-frame %.3f max %.3f ms lr %llx args %llx,%llx,%llx,%llx\n",
            (unsigned long long)frame,(unsigned long long)frames,(unsigned long long)(t_cur?t_cur->id:0),
            i==WAIT_PROBE_CORE0?"core0-":i==WAIT_PROBE_HOST?"host-":"svc-",i,
            (unsigned long long)wait_probe.calls[i],wait_probe.ns[i]/1e6,frames?wait_probe.ns[i]/1e6/frames:0,wait_probe.max_ns[i]/1e6,
            (unsigned long long)wait_probe.lr[i],(unsigned long long)wait_probe.args[i][0],(unsigned long long)wait_probe.args[i][1],
            (unsigned long long)wait_probe.args[i][2],(unsigned long long)wait_probe.args[i][3]);
    }
    if(wait_probe.calls[0x34])LOG("[kernel-edge] frame %llu thread %llu longest address wait %llx woken by thread %llu lr %llx\n",
        (unsigned long long)frame,(unsigned long long)(t_cur?t_cur->id:0),(unsigned long long)wait_probe.args[0x34][0],
        (unsigned long long)wait_probe.address_waker,(unsigned long long)wait_probe.address_waker_lr);
    if(t_cur && t_cur->id==atomic_load_explicit(&wait_probe_presenter,memory_order_relaxed)) {
        // arbiter_wake publishes both fields under g_klock. Read the pair there
        // too, then log outside the kernel lock.
        pthread_mutex_lock(&g_klock);
        uint64_t signaler=atomic_load_explicit(&wait_probe_signaler,memory_order_relaxed);
        uint64_t lr=atomic_load_explicit(&wait_probe_signal_lr,memory_order_relaxed);
        pthread_mutex_unlock(&g_klock);
        LOG("[kernel-wake] frame %llu last address signaler thread %llu lr %llx\n",
            (unsigned long long)frame,(unsigned long long)signaler,(unsigned long long)lr);
    }
    memset(&wait_probe,0,sizeof wait_probe);
    wait_probe.since=now;wait_probe.frame=frame;wait_probe.cpu_ns=cpu_ns;
}
void kernel_wait_probe_frame(uint64_t frame) {
    if(!wait_probe_on())return;
    atomic_store_explicit(&wait_probe_presenter,t_cur?t_cur->id:0,memory_order_relaxed);
    atomic_store_explicit(&wait_probe_frame,frame,memory_order_relaxed);
    wait_probe_report(frame);
}
#else
#define WAIT_PROBE_CORE0 0x80
#define WAIT_PROBE_HOST 0x81
static inline void wait_probe_add(unsigned kind,uint64_t ns) { (void)kind;(void)ns; }
#endif

static bool kwait(int64_t timeout_ns, const struct timespec *ts) {
    pthread_cond_t *cv = t_cur ? &t_cur->cond : &g_nothread_cond;
    uint64_t t0 = plat_now_ns();
    bool r = true;
#if TOTK_DIAGNOSTICS
    if(wait_probe_on() && t_cur && t_cur->svc_now==0x35)
        wait_probe_wakers[t_cur->id%256].target=0;
#endif
    if (timeout_ns < 0) pthread_cond_wait(cv, &g_klock);
    else r = pthread_cond_timedwait(cv, &g_klock, ts) == 0;
    uint64_t elapsed=plat_now_ns()-t0;
    g_blocked_ns += elapsed;
    if(t_cur && t_cur->svc_now)wait_probe_add(t_cur->svc_now-1,elapsed);
#if TOTK_DIAGNOSTICS
    wait_probe_long_record(t0,t0+elapsed);
#endif
    return r;
}

// ------------------------------------------------------------------ memory map
typedef struct { uint64_t addr, size; uint32_t type, perm; bool physical; } Region;
enum { MAX_REGIONS = 8192 };
static Region regions[MAX_REGIONS];
static pthread_mutex_t g_memlock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t system_resource_size;
static int nregions;
static _Atomic uint64_t heap_size;
#ifdef __ANDROID__
static uint64_t tls_next;  // (TLS_BASE moves with the window: kernel_init sets it)
#else
static uint64_t tls_next = TLS_BASE;
#endif
uint64_t kernel_heap_size(void) { return heap_size; }  // bytes from HEAP_BASE set by SetHeapSize
// Writable guest data regions (heap, mapped/physical memory, module .data): for debug/runtime memory scans.
int kernel_data_regions(uint64_t *addr, uint64_t *size, int max) {
    pthread_mutex_lock(&g_memlock);
    int n = 0;
    for (int i = 0; i < nregions && n < max; i++)
        if ((regions[i].perm & 2) && regions[i].type != 0xB && regions[i].type != 0xC) {
            addr[n] = regions[i].addr; size[n] = regions[i].size; n++;
        }
    pthread_mutex_unlock(&g_memlock);
    return n;
}

// [addr, addr+size) inside the guest window, without overflowing addr+size.
static bool guest_range_ok(uint64_t addr, uint64_t size) {
    return addr >= GUEST_LO && addr <= GUEST_HI && size <= GUEST_HI - addr;
}

static int region_add_locked(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    if (nregions == MAX_REGIONS) FATAL("too many regions\n");
    plat_commit(addr, size);  // back it before the OS writes there (Windows: file reads into the heap)
    int i = nregions++;
    while (i > 0 && regions[i - 1].addr > addr) { regions[i] = regions[i - 1]; i--; }
    regions[i] = (Region){addr, size, type, perm, false};
    return i;
}
static void region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    pthread_mutex_lock(&g_memlock);
    region_add_locked(addr, size, type, perm);
    pthread_mutex_unlock(&g_memlock);
}

static void region_remove_locked(uint64_t addr) {
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr) {
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Region));
            nregions--;
            return;
        }
    }
}

static void region_remove(uint64_t addr) {
    pthread_mutex_lock(&g_memlock);
    region_remove_locked(addr);
    pthread_mutex_unlock(&g_memlock);
}

static void query_memory_locked(uint64_t addr, uint64_t out) {
    uint64_t prev_end = 0;
    uint64_t info[5] = {0};  // addr, size, type|attr<<32, perm|ipc<<32, device|pad<<32
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr < r->addr + r->size) {
            info[0] = r->addr;
            info[1] = r->size;
            info[2] = r->type;
            info[3] = r->perm;
            memcpy((void *)out, info, 0x28);
            return;
        }
        if (addr < r->addr) {
            info[0] = prev_end;
            info[1] = r->addr - prev_end;
            memcpy((void *)out, info, 0x28);
            return;
        }
        prev_end = r->addr + r->size;
    }
    info[0] = prev_end;
    info[1] = (1ull << 39) - prev_end;
    memcpy((void *)out, info, 0x28);
}

static void query_memory(uint64_t addr, uint64_t out) {
    pthread_mutex_lock(&g_memlock);
    query_memory_locked(addr, out);
    pthread_mutex_unlock(&g_memlock);
}

void kernel_set_system_resource_size(uint64_t size) {
    if (size > 0x40000000 || (size & 0xfff)) FATAL("invalid NPDM system resource size\n");
    system_resource_size = size;
    LOG("[kernel] NPDM system resources: %#llx bytes\n", (unsigned long long)size);
}
static uint64_t memory_total(void) {
    const char *e = getenv("LARECOMP_TOTAL_MEM");
    return e ? strtoull(e, NULL, 16) : 0xCD500000ull;
}
static uint64_t memory_extra(void) {
    const char *e = getenv("LARECOMP_USED_EXTRA");
    return e ? strtoull(e, NULL, 16) : 0x4000000ull;
}
static uint64_t physical_used_locked(void) {
    uint64_t bytes = 0;
    for (int i = 0; i < nregions; i++) if (regions[i].physical) bytes += regions[i].size;
    return bytes;
}
static uint64_t physical_used(void) {
    pthread_mutex_lock(&g_memlock);
    uint64_t bytes = physical_used_locked();
    pthread_mutex_unlock(&g_memlock);
    return bytes;
}
static void merge_physical_locked(void) {
    for (int i = 1; i < nregions;) {
        Region *a = &regions[i - 1], *b = &regions[i];
        if (a->physical && b->physical && a->addr + a->size == b->addr) {
            a->size += b->size;
            memmove(b, b + 1, (nregions - i - 1) * sizeof *b);
            nregions--;
        } else i++;
    }
}
static uint32_t physical_memory_apply(uint64_t addr, uint64_t size, bool map) {
    if (!system_resource_size) return R_INVALID_STATE;
    if (!size || ((addr | size) & 0xfff) || size > ALIAS_SIZE || addr < ALIAS_BASE ||
        addr - ALIAS_BASE > ALIAS_SIZE - size) return R_INVALID_ADDRESS;
    uint64_t end = addr + size, added = size;
    pthread_mutex_lock(&g_memlock);
    uint32_t result = R_OK;
    // Validate the whole request before changing either contents or the map.
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        uint64_t re = r->addr + r->size;
        if (re <= addr || r->addr >= end) continue;
        if (!r->physical) { result = R_INVALID_STATE; goto done; }
        added -= (re < end ? re : end) - (r->addr > addr ? r->addr : addr);
    }
    if (map) {
        uint64_t used = heap_size + memory_extra() + system_resource_size + physical_used_locked();
        if (used > memory_total() || added > memory_total() - used) { result = R_OUT_OF_RESOURCE; goto done; }
        for (uint64_t cursor = addr; cursor < end;) {
            uint64_t next = end;
            bool covered = false;
            for (int i = 0; i < nregions; i++) {
                Region *r = &regions[i];
                if (r->addr + r->size <= cursor) continue;
                if (r->addr <= cursor) { cursor = r->addr + r->size; covered = true; }
                else if (r->addr < next) next = r->addr;
                break;
            }
            if (covered) continue;
            if (nregions == MAX_REGIONS) { result = R_OUT_OF_RESOURCE; goto done; }
            int i = region_add_locked(cursor, next - cursor, 5, 3);
#ifdef __ANDROID__
            extern bool vk_shared_memory_place(uint64_t addr, uint64_t size);
            vk_shared_memory_place(cursor, next - cursor);
#endif
            memset((void *)(uintptr_t)cursor, 0, next - cursor);
            regions[i].physical = true;
            merge_physical_locked();
            cursor = next;
        }
    } else {
        if (nregions == MAX_REGIONS) { result = R_OUT_OF_RESOURCE; goto done; }
#ifdef __ANDROID__
        extern bool vk_shared_memory_unmap(uint64_t addr, uint64_t size);
        if (!vk_shared_memory_unmap(addr, size)) { result = R_OUT_OF_RESOURCE; goto done; }
#endif
        // Release only whole host pages wholly inside the unmapped interval.
        // Guest pages at either edge are cleared if subsequently mapped again.
        uint64_t lo = (addr + PLAT_PAGE - 1) & ~(PLAT_PAGE - 1), hi = end & ~(PLAT_PAGE - 1);
#ifdef __ANDROID__
        extern bool vk_shared_memory_reset_cpu(uint64_t lo, uint64_t hi);
        if (lo < hi && !vk_shared_memory_reset_cpu(lo, hi)) { result = R_OUT_OF_RESOURCE; goto done; }
#else
        if (lo < hi && !plat_reset_guest(lo, hi)) { result = R_OUT_OF_RESOURCE; goto done; }
#endif
        for (int i = 0; i < nregions;) {
            Region r = regions[i];
            uint64_t re = r.addr + r.size;
            if (re <= addr || r.addr >= end) { i++; continue; }
            if (r.addr < addr && re > end) {
                memmove(&regions[i + 2], &regions[i + 1], (nregions - i - 1) * sizeof r);
                regions[i].size = addr - r.addr;
                regions[i + 1] = (Region){end, re - end, r.type, r.perm, true};
                nregions++; i += 2;
            } else if (r.addr < addr) { regions[i].size = addr - r.addr; i++; }
            else if (re > end) { regions[i].addr = end; regions[i].size = re - end; i++; }
            else { memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof r); nregions--; }
        }
    }
done:
    pthread_mutex_unlock(&g_memlock);
    return result;
}

#ifdef __ANDROID__
typedef struct { uint64_t addr, size; uint32_t result; } PhysicalRelease;
static void physical_memory_release(void *arg) {
    PhysicalRelease *r = arg;
    r->result = physical_memory_apply(r->addr, r->size, false);
}
#endif
static uint32_t physical_memory(uint64_t addr, uint64_t size, bool map) {
#ifdef __ANDROID__
    extern bool vk_shared_memory_enabled(void);
    extern void vk_async_call_sync(void (*call)(void *), void *arg);
    if (!map && vk_shared_memory_enabled()) {
        // Even with no current regions, a concurrent mapper can create the
        // first backing before this call obtains g_memlock. Select queue order
        // from the fixed startup option, not the racy current region count.
        // Enter render-queue order before taking g_memlock. Prior translation
        // can query guest mappings; waiting on it while holding the kernel's
        // map lock would deadlock. Validation and release remain atomic below.
        PhysicalRelease r = {addr, size, R_OUT_OF_RESOURCE};
        vk_async_call_sync(physical_memory_release, &r);
        return r.result;
    }
#endif
    return physical_memory_apply(addr, size, map);
}

// ------------------------------------------------------------------ threads
static uint64_t next_thread_id = 0x50;

// ------------------------------------------------------------------ in-place reboot
// Recompiled guest code can't be killed from outside, so a reboot (runtime/main.c) sets g_rt_unwind, wakes every
// waiting thread, and each guest thread longjmps back to thread_main at its next safe point: rt_svc (entry and
// exit), an HLE call (runtime/dispatch.c), or a kernel wait (which returns early while g_rt_unwind is set). The
// frames dropped are recompiled code (its state lives in guest memory, which is wiped next) and HLE functions;
// none of those hold a host lock across a call into the guest, so nothing stays locked. svcExitThread ends a
// thread the same way.
volatile int g_rt_unwind;
static KThread *g_threads;  // every thread of this boot (g_klock)

void rt_unwind_point(void) {
    if (__builtin_expect(g_rt_unwind, 0) && t_cur) longjmp(t_cur->unwind, 1);
}

#ifdef __ANDROID__
// ---- core 0, one thread at a time (LARECOMP_NO_CORE0=1: off)
// On the Switch the threads of one core take turns: one runs until it blocks in the kernel (a wait, a sleep, a yield,
// an IPC request), and only then does another of that core run. The game relies on it. Its job workers are created
// for the main thread's core (core -2: the process's, 0) at the main thread's priority, so to the main thread a
// worker's "run the job, then tell its owner" is one step - here, with every thread truly parallel, the main thread
// sometimes moved on between the two and the worker told an owner that was gone (2026-10-16: crashes, and a hang
// when the notification was skipped instead: hle/job_race.c). So the threads of core 0 take turns here too: a runnable
// queue, held while the thread runs and handed on at its blocking SVCs (and around the host's long waits for the
// renderer: kernel_blocking_begin / _end). Cores 1-3 stay parallel, as on the Switch.
static pthread_mutex_t c0_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c0_cv = PTHREAD_COND_INITIALIZER;
static KThread *c0_head, *c0_tail;
static int c0_on = -1;
static bool c0_priority; // TotK policy; equal-priority threads remain FIFO
static KThread *c0_holder;  // (c0_m) the thread that has core 0, since c0_since
static KThread *c0_priority_preempted; // preserve an interrupted owner's place among equal-priority peers
static uint64_t c0_since;
// Private per-boot state avoids changing the game-library ABI. IDs only grow
// within a boot, just like g_threads. Writers hold both g_klock and c0_m;
// readers hold either lock. The thread pointer rejects stale/test-fixture IDs.
typedef struct { KThread *thread; int value, previous; } CorePriority;
static CorePriority *c0_priorities;
static size_t c0_priority_capacity;
static unsigned c0_mutex_waiters; // g_klock
#if TOTK_DIAGNOSTICS
static unsigned c0_hold_reports; // c0_m; normal-release diagnostics only
#endif

// Optional cooperative slices: request a handoff at the next function entry/SVC, never run two owners at once.
// BOTW's busy waits depend on workers on other cores, handled by SetThreadCoreMask, not by forcing a handoff.
// (LARECOMP_C0_SLICE_MS: the slice; default 0)
extern volatile int g_fnprof;  // runtime/fnprof.c
static int c0_nevicted;  // (c0_m)
static uint64_t c0_slice_ns;  // affinity handles BOTW's cross-core waits; slicing is diagnostic only
static int thread_effective_priority(const KThread *t) {
    if (c0_priority && g_game_version == TOTK_VERSION && t->id < c0_priority_capacity &&
        c0_priorities[t->id].thread == t) return c0_priorities[t->id].value;
    return thread_priority(t);
}
// Both locks held. Recompute the minimum priority along actual mutex-owner
// edges, including chains. Starting from base priorities also removes donations
// after handoff, cancellation, timeout or a base-priority change.
static void core0_update_inheritance(KThread *changed) {
    if (!c0_priority || g_game_version != TOTK_VERSION) return;
    size_t needed = 0, threads = 0;
    for (KThread *t = g_threads; t; t = t->anext) {
        if (t->id >= SIZE_MAX / sizeof(CorePriority)) FATAL("kernel: priority state is too large\n");
        if (needed <= t->id) needed = (size_t)t->id + 1;
        ++threads;
    }
    if (needed > c0_priority_capacity) {
        size_t capacity = c0_priority_capacity ? c0_priority_capacity : 256;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / sizeof(CorePriority) / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        CorePriority *entries = realloc(c0_priorities, capacity * sizeof *entries);
        if (!entries) FATAL("kernel: cannot allocate priority state\n");
        memset(entries + c0_priority_capacity, 0, (capacity - c0_priority_capacity) * sizeof *entries);
        c0_priorities = entries;
        c0_priority_capacity = capacity;
    }
    for (KThread *t = g_threads; t; t = t->anext) {
        int previous = thread_effective_priority(t);
        c0_priorities[t->id] = (CorePriority){t, thread_priority(t), previous};
    }
    c0_mutex_waiters = 0;
    for (KThread *w = g_waitlist; w; w = w->wnext) c0_mutex_waiters += w->wait_kind == WAIT_MUTEX;
    for (size_t pass = 0; pass < threads && c0_mutex_waiters; ++pass) {
        bool updated = false;
        for (KThread *w = g_waitlist; w; w = w->wnext) {
            if (w->wait_kind != WAIT_MUTEX) continue;
            uint32_t owner = __atomic_load_n((uint32_t *)(uintptr_t)w->wait_addr, __ATOMIC_SEQ_CST) & ~WAITERS_BIT;
            KObject *object = handle_get(owner);
            if (!object || object->type != KOBJ_THREAD) continue;
            KThread *t = (KThread *)object;
            if (t->id >= c0_priority_capacity || c0_priorities[t->id].thread != t) continue;
            int priority = thread_effective_priority(w);
            if (c0_priorities[t->id].value > priority) {
                c0_priorities[t->id].value = priority;
                updated = true;
            }
        }
        if (!updated) break;
    }
    // Changed queued priorities join the end of their new group. The explicit
    // SetThreadPriority target is handled by its caller, including new threads.
    KThread *moved = NULL, *moved_tail = NULL, *kept_tail = NULL;
    for (KThread **link = &c0_head; *link;) {
        KThread *t = *link;
        bool registered = t->id < c0_priority_capacity && c0_priorities[t->id].thread == t;
        if (t != changed && registered && c0_priorities[t->id].previous != c0_priorities[t->id].value) {
            *link = t->core0_next;t->core0_next = NULL;
            if (moved_tail) moved_tail->core0_next = t;else moved = t;
            moved_tail = t;
        } else { kept_tail = t;link = &t->core0_next; }
    }
    if (kept_tail) kept_tail->core0_next = moved;else c0_head = moved;
    c0_tail = moved_tail ? moved_tail : kept_tail;
}
// c0_m held. Ownership stays with the old thread until it reaches a safe point.
static void core0_request_preempt(KThread *t) {
    if (!t->core0_evicted) {
        __atomic_store_n(&t->core0_evicted, true, __ATOMIC_RELEASE);
        if (c0_nevicted++ == 0) __atomic_fetch_or(&g_fnprof, 2, __ATOMIC_SEQ_CST);
    }
}
static KThread *core0_front(void) {
    if (!c0_priority || g_game_version != TOTK_VERSION) return c0_head;
    KThread *best = NULL;
    for (KThread *t = c0_head; t; t = t->core0_next)
        if (__atomic_load_n(&t->core, __ATOMIC_ACQUIRE) == 0 &&
            (!best || thread_effective_priority(t) < thread_effective_priority(best))) best = t;
    return best;
}
static void core0_reconsider(void) {
    if (!c0_priority || g_game_version != TOTK_VERSION || !c0_holder) return;
    KThread *next = core0_front();
    if (next && thread_effective_priority(next) < thread_effective_priority(c0_holder)) {
        if (!c0_holder->core0_evicted) c0_priority_preempted = c0_holder;
        core0_request_preempt(c0_holder);
    }
}
// g_klock held; never introduce c0_m -> g_klock acquisition.
static void core0_refresh_inheritance(bool added) {
    if (!c0_priority || g_game_version != TOTK_VERSION || (!added && !c0_mutex_waiters)) return;
    pthread_mutex_lock(&c0_m);
    core0_update_inheritance(NULL);
    core0_reconsider();
    pthread_cond_broadcast(&c0_cv);
    pthread_mutex_unlock(&c0_m);
}
static void *core0_slicer(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&c0_m);
        if (c0_holder && c0_head && !c0_holder->core0_evicted && plat_now_ns() - c0_since > c0_slice_ns) {
            static uint64_t n;
            if (n++ < 10 || DIAG_ENV("LARECOMP_LOG_C0"))
                LOG("[kernel] core 0: t%llu ran %llums without blocking, handoff requested\n", (unsigned long long)c0_holder->id,
                    (unsigned long long)((plat_now_ns() - c0_since) / 1000000));
            core0_request_preempt(c0_holder);
            // The old owner must reach a safe point before another thread can run. Merely clearing the owner
            // here lets both execute guest code at once, corrupting the game's supposedly serial work queues.
        }
        pthread_mutex_unlock(&c0_m);
    }
    return NULL;
}

static pthread_once_t c0_once = PTHREAD_ONCE_INIT;
static void core0_init(void) {
    c0_on = !getenv("LARECOMP_NO_CORE0");
    c0_priority = true; // Version checks stay at use sites, including after a game reboot.
#if TOTK_DIAGNOSTICS
    const char *priority = diag_getenv("LARECOMP_C0_PRIORITY");
    if (priority) c0_priority = atoi(priority) != 0;
#endif
    if (c0_on && c0_priority && g_game_version == TOTK_VERSION)
        LOG("[kernel] core 0: guest priority scheduling with safe-point preemption\n");
    const char *e = getenv("LARECOMP_C0_SLICE_MS");
    if (e && atoi(e) > 0) c0_slice_ns = (uint64_t)atoi(e) * 1000000;
    if (c0_on && c0_slice_ns) {
        pthread_t th;
        pthread_create(&th, NULL, core0_slicer, NULL);
        pthread_detach(th);
    }
}
static void core0_acquire(KThread *t) {
    pthread_once(&c0_once, core0_init);
    if (!t || !c0_on) return;
    pthread_mutex_lock(&c0_m);
    bool resume_priority = c0_priority_preempted == t;
    if (resume_priority) c0_priority_preempted = NULL;
    if (t->core0_evicted) {
        __atomic_store_n(&t->core0_evicted, false, __ATOMIC_RELEASE);
        if (--c0_nevicted == 0) __atomic_fetch_and(&g_fnprof, ~2, __ATOMIC_SEQ_CST);
        if (t->core0_held) {
            t->core0_held = false;
            c0_holder = NULL;
            pthread_cond_broadcast(&c0_cv);
        }
    }
    if (__atomic_load_n(&t->core, __ATOMIC_ACQUIRE) != 0) {
        if (t->core0_held) {
            t->core0_held = false;
            c0_holder = NULL;
            pthread_cond_broadcast(&c0_cv);
        }
        pthread_mutex_unlock(&c0_m);
        return;
    }
    if (t->core0_held) { pthread_mutex_unlock(&c0_m); return; }
    // A removable queue lets SetThreadCoreMask move a waiter to another core without waiting for the current
    // owner to yield (that owner may itself be spinning for this worker).
    if (resume_priority) {
        t->core0_next = c0_head;
        c0_head = t;
        if (!c0_tail) c0_tail = t;
    } else {
        t->core0_next = NULL;
        if (c0_tail) c0_tail->core0_next = t;
        else c0_head = t;
        c0_tail = t;
    }
    core0_reconsider();
    uint64_t t0 = plat_now_ns();
    while ((c0_holder || core0_front() != t) && __atomic_load_n(&t->core, __ATOMIC_ACQUIRE) == 0 && !g_rt_unwind)
        pthread_cond_wait(&c0_cv, &c0_m);
    uint64_t elapsed=plat_now_ns()-t0;
    g_blocked_ns += elapsed;
    wait_probe_add(WAIT_PROBE_CORE0,elapsed);
    KThread **p = &c0_head, *prev = NULL;
    while (*p != t) { prev = *p; p = &(*p)->core0_next; }
    *p = t->core0_next;
    if (c0_tail == t) c0_tail = prev;
    t->core0_next = NULL;
    if (__atomic_load_n(&t->core, __ATOMIC_ACQUIRE) != 0 || g_rt_unwind) {
        pthread_cond_broadcast(&c0_cv);
        pthread_mutex_unlock(&c0_m);
        return;
    }
    t->core0_held = true;
    c0_holder = t;
    c0_since = plat_now_ns();
    pthread_mutex_unlock(&c0_m);
}

// Apply a pending migration or cooperative handoff at a guest safe point.
void kernel_preempt_point(void) {
    KThread *t = t_cur;
    if (t && __atomic_load_n(&t->core0_evicted, __ATOMIC_ACQUIRE)) core0_acquire(t);
}

static void core0_release(KThread *t) {
    if (!t) return;
#if TOTK_DIAGNOSTICS
    uint64_t trace_start=0,trace_end=0,trace_next=0,trace_frame=0;
#endif
    pthread_mutex_lock(&c0_m);
    if (c0_priority_preempted == t) c0_priority_preempted = NULL;
    if (t->core0_evicted) {
        __atomic_store_n(&t->core0_evicted, false, __ATOMIC_RELEASE);
        if (--c0_nevicted == 0) __atomic_fetch_and(&g_fnprof, ~2, __ATOMIC_SEQ_CST);
    }
    if (t->core0_held) {
        t->core0_held = false;
        if (c0_holder == t) {
#if TOTK_DIAGNOSTICS
            if(c0_head && c0_hold_reports<128 && wait_probe_on()) {
                trace_frame=atomic_load_explicit(&wait_probe_frame,memory_order_relaxed);
                if(trace_frame>=wait_probe_long_first()) {
                    uint64_t now=plat_now_ns();
                    if(now-c0_since>=10000000ull) {
                        KThread *next=core0_front();
                        trace_start=c0_since;trace_end=now;trace_next=next?next->id:0;
                        ++c0_hold_reports;
                    }
                }
            }
#endif
            c0_holder = NULL;
        }
        pthread_cond_broadcast(&c0_cv);
    }
    pthread_mutex_unlock(&c0_m);
#if TOTK_DIAGNOSTICS
    if(trace_end)LOG("[kernel-core0-hold] frame %llu holder %llu next %llu ns %llu..%llu wall %.3f ms\n",
        (unsigned long long)trace_frame,(unsigned long long)t->id,(unsigned long long)trace_next,
        (unsigned long long)trace_start,(unsigned long long)trace_end,(trace_end-trace_start)/1e6);
#endif
}

// The SVCs a thread blocks in on the Switch (the core goes to another of its threads meanwhile)
static bool svc_blocks(uint32_t imm) {
    switch (imm) {
    case 0x0A: case 0x0B: case 0x18: case 0x1A: case 0x1C: case 0x21: case 0x22: case 0x34: case 0x43: return true;
    default: return false;
    }
}

// SleepThread(0/-1) yields to eligible guest threads. With a core-0-only
// affinity, rt_svc's release/reacquire already queues this thread behind its
// peers; there is no permitted guest migration for the host scheduler to do.
// TotK's BaseProcExecutor0 polls with -1 hundreds of times per frame. The host
// yield adds latency without changing this FIFO handoff. Other games retain
// their existing path; diagnostics can compare either implementation.
static bool core0_guest_yield(int64_t ns) {
    bool enabled=g_game_version==TOTK_VERSION;
#if TOTK_DIAGNOSTICS
    static _Atomic int override=-2;
    int value=atomic_load_explicit(&override,memory_order_relaxed);
    if(value==-2) {
        const char *e=diag_getenv("LARECOMP_C0_GUEST_YIELD");value=e?atoi(e)!=0:-1;
        atomic_store_explicit(&override,value,memory_order_relaxed);
    }
    if(value>=0)enabled=value!=0;
#endif
    if(!enabled || !c0_on || !t_cur || (ns!=0 && ns!=-1))return false;
    pthread_mutex_lock(&g_klock);
    bool pinned=t_cur->core_mask==1 && __atomic_load_n(&t_cur->core,__ATOMIC_ACQUIRE)==0;
    pthread_mutex_unlock(&g_klock);
    return pinned;
}

// The host's own long waits on a guest thread (the GPU, the render thread): core 0 goes to its other threads meanwhile
// (the wait counts as blocked, as a kernel wait: the frame pacer measures the presenting thread's own work)
static __thread uint64_t blocking_t0;
void kernel_blocking_begin(void) {
    blocking_t0 = plat_now_ns();
    core0_release(t_cur);
}
void kernel_blocking_end(void) {
    if(blocking_t0)wait_probe_add(WAIT_PROBE_HOST,plat_now_ns()-blocking_t0);
    core0_acquire(t_cur);
    if (blocking_t0) g_blocked_ns += plat_now_ns() - blocking_t0;
    blocking_t0 = 0;
}
#else
static int thread_effective_priority(const KThread *t) { return thread_priority(t); }
static void core0_refresh_inheritance(bool added) { (void)added; }
#endif

static void *thread_main(void *arg) {
    KThread *t = arg;
    t_cur = t;
#ifdef __ANDROID__
    plat_thread_unpin();  // (not the presenting thread's fast cores: platform.h)
#endif
    crash_handler_install();
    rt_set_fpcr(0);
#ifdef __ANDROID__
    if (!setjmp(t->unwind)) {
        core0_acquire(t);
        rt_unwind_point();
        guest_call(&t->cpu, t->entry);
    }
    core0_release(t);
#else
    if (!setjmp(t->unwind)) guest_call(&t->cpu, t->entry);
#endif
    pthread_mutex_lock(&g_klock);
    t->finished = true;
    kernel_signal(&t->obj);
    pthread_mutex_unlock(&g_klock);
    t_cur = NULL;
    return NULL;
}

static KThread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core) {
    KThread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cond, NULL);
    t->obj.type = KOBJ_THREAD;
    t->entry = entry;
    __atomic_store_n(&t->prio, prio, __ATOMIC_RELAXED);
    t->core = core < 0 ? 0 : core;
    t->ideal_core = t->core;
    t->core_mask = 1ull << t->core;
    t->cpu.x[0] = arg;
    t->cpu.sp = stack_top & ~15ull;
    t->cpu.excl_addr = ~0ull;
    t->cpu.thread = t;
    pthread_mutex_lock(&g_klock);
    t->id = next_thread_id++;
    t->tlr = tls_next;
    tls_next += 0x200;
    t->cpu.tpidrro = t->tlr;
    t->anext = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_klock);
    return t;
}

// Caller holds g_klock. Core ownership changes only at a guest safe point.
static uint32_t thread_set_core_mask(KThread *t, int ideal, uint64_t mask) {
    uint32_t result = R_OK;
    if (ideal == -2) { ideal = 0; mask = 1; }  // process default (GetInfo 20)
    if (ideal == -3) ideal = t->ideal_core;     // preserve preference
    if (ideal < -1 || ideal > 3 || (mask & ~0xFull)) result = R_INVALID_CORE;
    else if (!mask || (ideal >= 0 && !(mask & (1ull << ideal)))) result = R_INVALID_COMBINATION;
    int old_core = __atomic_load_n(&t->core, __ATOMIC_ACQUIRE), core = old_core;
    if (!result) {
        if (!(mask & (1ull << core))) core = ideal >= 0 ? ideal : __builtin_ctzll(mask);
        t->ideal_core = ideal;
        t->core_mask = mask;
#ifdef __ANDROID__
        pthread_mutex_lock(&c0_m);
#endif
        __atomic_store_n(&t->core, core, __ATOMIC_RELEASE);
#ifdef __ANDROID__
        if (core != old_core && t->started && c0_on && !t->core0_evicted) {
            core0_request_preempt(t);
        }
        core0_reconsider();
        pthread_cond_broadcast(&c0_cv);
        pthread_mutex_unlock(&c0_m);
#endif
    }
    return result;
}

// Caller holds g_klock, matching the affinity update's g_klock -> c0_m order.
static void thread_set_priority(KThread *t, int priority) {
#ifdef __ANDROID__
    pthread_mutex_lock(&c0_m);
#endif
    int previous = thread_effective_priority(t);
    __atomic_store_n(&t->prio, priority, __ATOMIC_RELAXED);
#ifdef __ANDROID__
    core0_update_inheritance(t);
    if (previous != thread_effective_priority(t) && c0_priority && g_game_version == TOTK_VERSION) {
        // A queued priority change joins the end of its new priority group.
        KThread **link = &c0_head;
        while (*link && *link != t) link = &(*link)->core0_next;
        if (*link && c0_tail != t) {
            *link = t->core0_next;
            c0_tail->core0_next = t;
            c0_tail = t;
            t->core0_next = NULL;
        }
    }
    core0_reconsider();
    pthread_cond_broadcast(&c0_cv);
    pthread_mutex_unlock(&c0_m);
#endif
    (void)previous;
}

static void thread_start(KThread *t) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256ull << 20);  // guest recursion runs on the host stack
    t->started = true;
    pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
}

KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio) {
    uint64_t stack = STACK_BASE + STACK_SIZE - ((stack_size + 0xFFFF) & ~0xFFFFull);
    region_add(stack, stack_size, 0xB, 3);
    KThread *t = thread_new(entry, 0, stack + stack_size, prio, 0);
    t->handle = kernel_new_handle(&t->obj);
    return t;
}

void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4) {
    t->cpu.x[0] = x0; t->cpu.x[1] = x1; t->cpu.x[2] = x2; t->cpu.x[3] = x3; t->cpu.x[4] = x4;
    thread_start(t);  // the host main thread goes on to run the window event loop
}

uint32_t kernel_current_thread_handle(void) { return t_cur ? t_cur->handle : 0; }
Cpu *kernel_current_cpu(void) { return t_cur ? &t_cur->cpu : NULL; }
KThread *kernel_current_thread(void) { return t_cur; }

// ------------------------------------------------------------------ mutex / condvar (Horizon semantics)
// Mutex word: owner handle | WAITERS_BIT. Waiting threads sleep on the global cond and are handed ownership.

static uint32_t *u32p(uint64_t a) { return (uint32_t *)(uintptr_t)a; }

// Try to take mutex `m` for `tag`, or mark it contended. Returns true if acquired. Uses CAS so a
// concurrent user-space unlock (owner storing 0) is never overwritten.
static bool mutex_acquire_or_mark(uint64_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = __atomic_load_n(u32p(m), __ATOMIC_SEQ_CST);
        if (v == 0) {
            if (__atomic_compare_exchange_n(u32p(m), &v, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
        } else {
            if ((v & WAITERS_BIT) ||
                __atomic_compare_exchange_n(u32p(m), &v, v | WAITERS_BIT, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return false;
        }
    }
}

// Pick a thread waiting on mutex `addr` and give it ownership. Caller holds g_klock.
static void mutex_release(uint64_t addr) {
    KThread *next = NULL;
    int count = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (t->wait_kind == WAIT_MUTEX && t->wait_addr == addr) {
            if (!next || thread_effective_priority(t) < thread_effective_priority(next)) next = t;
            count++;
        }
    }
    if (!next) {
        __atomic_store_n(u32p(addr), 0, __ATOMIC_SEQ_CST);
        core0_refresh_inheritance(false);
        return;
    }
    __atomic_store_n(u32p(addr), next->handle | (count > 1 ? WAITERS_BIT : 0), __ATOMIC_SEQ_CST);
    next->wait_kind = WAIT_NONE;
    next->wait_result = R_OK;
    wake(next);
    core0_refresh_inheritance(false);
}

KThread *g_waitlist;

static void waitlist_add(KThread *t) {
    t->wnext = g_waitlist;g_waitlist = t;
    if (t->wait_kind == WAIT_MUTEX) core0_refresh_inheritance(true);
}
static void waitlist_remove(KThread *t) {
    for (KThread **p = &g_waitlist; *p; p = &(*p)->wnext)
        if (*p == t) { *p = t->wnext;core0_refresh_inheritance(false);return; }
}

// Block until wait_kind is cleared by a waker, timeout, or cancel. Caller holds g_klock.
static uint32_t block_current(int64_t timeout_ns) {
    KThread *t = t_cur;
    struct timespec ts;
    if (timeout_ns >= 0) deadline(&ts, timeout_ns);
    waitlist_add(t);
#ifdef __ANDROID__
    int kind0 = t->wait_kind;
#endif
    while (t->wait_kind != WAIT_NONE) {
        if (g_rt_unwind) {  // reboot: give up the wait (the thread unwinds when its svc returns)
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (t->cancel) {
            t->cancel = false;
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (!kwait(timeout_ns, &ts)) {
#ifdef __ANDROID__
            // the wait ended while the timeout fired: a waker got there first (woken: wait_kind is NONE and the
            // loop ends with its result - a condition variable's thread may own the mutex by now) or moved it on
            // (a condition variable's thread signalled, now waiting for its mutex: no timeout for that part)
            if (t->wait_kind != kind0) {
                timeout_ns = -1;
                continue;
            }
#endif
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_TIMEOUT;
            break;
        }
    }
    waitlist_remove(t);
    return t->wait_result;
}

static uint32_t svc_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    pthread_mutex_lock(&g_klock);
    uint32_t v = __atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (v != (owner | WAITERS_BIT)) {
        pthread_mutex_unlock(&g_klock);
        return R_OK;
    }
    KThread *t = t_cur;
    t->wait_kind = WAIT_MUTEX;
    t->wait_addr = addr;
    t->wait_tag = tag;
    uint32_t r = block_current(-1);
    pthread_mutex_unlock(&g_klock);
    return r == R_CANCELLED ? R_OK : r;
}

static uint32_t svc_arbitrate_unlock(uint64_t addr) {
    pthread_mutex_lock(&g_klock);
    mutex_release(addr);
    pthread_mutex_unlock(&g_klock);
    return R_OK;
}

static uint32_t svc_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    KThread *t = t_cur;
#ifdef __ANDROID__
    // the key says "waiters" before the mutex goes: once it's released, a thread that takes it, changes the state
    // and checks the key must see this waiter (else it skips the signal and the wakeup is lost)
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);
#endif
    mutex_release(mutex);
    t->wait_kind = WAIT_CONDVAR;
    t->wait_addr = key;
    t->wait_mutex = mutex;
    t->wait_tag = tag;
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);
    uint32_t r = block_current(timeout);
    // A wait that timed out returns without the mutex in BOTW 1.8.2's SDK, as on the console: user mode takes it
    // again (InternalConditionVariableImplByHorizon::TimedWait calls Enter on 0xea01). Taken here too, the thread then
    // waited for the mutex it held, and every thread after it (SavageWindRecomp). The older SDKs run as before.
    if (r != R_OK && !(r == R_TIMEOUT && g_game_version == TOTK_VERSION)) {
        // timed out / cancelled: still must reacquire the mutex
        if (!mutex_acquire_or_mark(mutex, tag)) {
            t->wait_kind = WAIT_MUTEX;
            t->wait_addr = mutex;
            block_current(-1);
        }
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void svc_signal_process_wide_key(uint64_t key, int32_t count) {
    pthread_mutex_lock(&g_klock);
    int woken = 0;
    bool any_left = false;
    for (;;) {
        if (count > 0 && woken >= count) break;
        KThread *best = NULL;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key && (!best || thread_effective_priority(t) < thread_effective_priority(best))) best = t;
        if (!best) break;
        woken++;
        // the woken thread must reacquire its mutex
        uint64_t m = best->wait_mutex;
        best->wait_result = R_OK;
        if (mutex_acquire_or_mark(m, best->wait_tag)) {
            best->wait_kind = WAIT_NONE;
            wake(best);
        } else {
            best->wait_kind = WAIT_MUTEX;
            best->wait_addr = m;
        }
    }
    for (KThread *t = g_waitlist; t; t = t->wnext)
        if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key) any_left = true;
    if (!any_left) __atomic_store_n(u32p(key), 0, __ATOMIC_SEQ_CST);
    core0_refresh_inheritance(true);
    pthread_mutex_unlock(&g_klock);
}

// ------------------------------------------------------------------ address arbiter
enum { ARB_WAIT_LESS = 0, ARB_DEC_WAIT_LESS = 1, ARB_WAIT_EQ = 2 };
enum { ARB_SIGNAL = 0, ARB_SIGNAL_INC_EQ = 1, ARB_SIGNAL_MOD_EQ = 2 };

static uint32_t svc_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    bool wait;
    if (type == ARB_WAIT_LESS) wait = cur < value;
    else if (type == ARB_DEC_WAIT_LESS) {
        wait = cur < value;
#ifdef __ANDROID__
        while (wait && !__atomic_compare_exchange_n(u32p(addr), (uint32_t *)&cur, (uint32_t)(cur - 1), false,
                                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            wait = cur < value;  // (cur reloaded by the failed exchange)
#else
        if (wait) __atomic_store_n(u32p(addr), (uint32_t)(cur - 1), __ATOMIC_SEQ_CST);
#endif
    } else if (type == ARB_WAIT_EQ) wait = cur == value;
    else { pthread_mutex_unlock(&g_klock); return R_INVALID_ENUM; }
    if (!wait) { pthread_mutex_unlock(&g_klock); return R_INVALID_STATE; }
    if (timeout == 0) { pthread_mutex_unlock(&g_klock); return R_TIMEOUT; }
    t_cur->wait_kind = WAIT_ARBITER;
    t_cur->wait_addr = addr;
    uint32_t r = block_current(timeout);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static int arbiter_wake(uint64_t addr, int32_t count) {
    int woken = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (count > 0 && woken >= count) break;
        if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) {
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_OK;
#if TOTK_DIAGNOSTICS
            if(wait_probe_on() && t->id==atomic_load_explicit(&wait_probe_presenter,memory_order_relaxed)) {
                atomic_store_explicit(&wait_probe_signaler,t_cur?t_cur->id:0,memory_order_relaxed);
                atomic_store_explicit(&wait_probe_signal_lr,t_cur?t_cur->svc_lr:0,memory_order_relaxed);
            }
            if(wait_probe_on()) {
                unsigned slot=(unsigned)(t->id%256);
                wait_probe_wakers[slot].target=t->id;
                wait_probe_wakers[slot].thread=t_cur?t_cur->id:0;
                wait_probe_wakers[slot].lr=t_cur?t_cur->svc_lr:0;
            }
#endif
            wake(t);
            woken++;
        }
    }
    return woken;
}

static uint32_t svc_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    pthread_mutex_lock(&g_klock);
    uint32_t r = R_OK;
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (type == ARB_SIGNAL) {
        arbiter_wake(addr, count);
    } else if (type == ARB_SIGNAL_INC_EQ) {
#ifdef __ANDROID__
        uint32_t expect = (uint32_t)value;
        if (!__atomic_compare_exchange_n(u32p(addr), &expect, (uint32_t)value + 1, false, __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST)) r = R_INVALID_STATE;
        else arbiter_wake(addr, count);
#else
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)(cur + 1), __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
#endif
    } else if (type == ARB_SIGNAL_MOD_EQ) {
        int waiting = 0;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) waiting++;
        int32_t nv;
        if (count <= 0) nv = waiting > 0 ? value - 2 : value + 1;
        else nv = waiting == 0 ? value + 1 : (waiting <= count ? value - 1 : value);
#ifdef __ANDROID__
        uint32_t expect = (uint32_t)value;
        if (!__atomic_compare_exchange_n(u32p(addr), &expect, (uint32_t)nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            r = R_INVALID_STATE;
        else arbiter_wake(addr, count);
#else
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)nv, __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
#endif
    } else {
        r = R_INVALID_ENUM;
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ wait synchronization

static uint32_t svc_wait_synchronization(uint64_t handles_ptr, int32_t count, int64_t timeout, int32_t *index) {
    pthread_mutex_lock(&g_klock);
    struct timespec ts;
    if (timeout > 0) deadline(&ts, timeout);
    KThread *t = t_cur;
    uint32_t *hs = (uint32_t *)(uintptr_t)handles_ptr;
    uint32_t r;
    if (DIAG_ENV("LARECOMP_VSYNC_BT")) {  // (debug) who waits for the display's vsync event: the game's frame loop
        extern uint32_t vi_vsync_handle(void);
        static int shown;
        for (int i = 0; i < count && shown < 2; i++)
            if (hs[i] && hs[i] == vi_vsync_handle()) {
                shown++;
                LOG("[vsync-bt] wait on vsync (%d handles, timeout %lld) lr %llx:\n", count, (long long)timeout,
                    (unsigned long long)(t ? t->cpu.x[30] : 0));
                plat_print_backtrace();
            }
    }
    for (;;) {
        for (int i = 0; i < count; i++) {
            KObject *o = handle_get(hs[i]);
            if (!o) { r = R_INVALID_HANDLE; goto out; }
            if (o->signaled) { *index = i; r = R_OK; goto out; }
        }
        if (t->cancel) { t->cancel = false; r = R_CANCELLED; goto out; }
        if (g_rt_unwind) { r = R_CANCELLED; goto out; }
        if (timeout == 0) { r = R_TIMEOUT; goto out; }
        t->in_sync_wait = true;
        t->snext = g_syncwaiters;
        g_syncwaiters = t;
        bool ok = kwait(timeout < 0 ? -1 : timeout, &ts);
        for (KThread **p = &g_syncwaiters; *p; p = &(*p)->snext)
            if (*p == t) { *p = t->snext; break; }
        t->in_sync_wait = false;
        if (!ok) { r = R_TIMEOUT; goto out; }
    }
out:
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ debug output
static void debug_string(uint64_t p, uint64_t len) {
    fprintf(stderr, "[guest] %.*s", (int)len, (const char *)(uintptr_t)p);
    if (len && ((const char *)(uintptr_t)p)[len - 1] != '\n') fputc('\n', stderr);
}

// ------------------------------------------------------------------ SVC dispatch
static int trace_svc = -1;

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc);

static _Atomic uint64_t svc_wait_ns[32][0x80], svc_calls[32][0x80];
static int prof_svc = -1;

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc);

// LARECOMP_WATCHDOG=<seconds>: every that many seconds, the game's threads stuck in one svc for longer, with its
// arguments and caller, and where each of the others is (its guest lr) - for hangs.
static void *watchdog_main(void *arg) {
    uint64_t every = (uint64_t)(uintptr_t)arg;
    for (;;) {
        sleep((unsigned)every);
        uint64_t now = plat_now_ns();
        LOG("[watchdog] threads:\n");
        for (KThread *t = g_threads; t; t = t->anext) {
            if (t->finished) continue;
            uint32_t n = t->svc_now;
            if (n && now - t->svc_since > every * 1000000000ull)
                LOG("[watchdog]  t%llu prio %d: svc %02x for %llus (%llx, %llx, %llx, %llx) from %llx; wait kind %d addr %llx\n",
                    (unsigned long long)t->id, thread_priority(t), n - 1, (unsigned long long)((now - t->svc_since) / 1000000000ull),
                    (unsigned long long)t->svc_args[0], (unsigned long long)t->svc_args[1],
                    (unsigned long long)t->svc_args[2], (unsigned long long)t->svc_args[3],
                    (unsigned long long)t->svc_lr, (int)t->wait_kind, (unsigned long long)t->wait_addr);
            else
                LOG("[watchdog]  t%llu prio %d: running%s, pc %llx lr %llx\n", (unsigned long long)t->id, thread_priority(t),
                    n ? " (short svc)" : "", (unsigned long long)t->cpu.pc, (unsigned long long)t->cpu.x[30]);
        }
    }
    return NULL;
}

static void watchdog_start(void) {
    static int started;
    const char *e = diag_getenv("LARECOMP_WATCHDOG");
    if (started || !e || atoi(e) <= 0) return;
    started = 1;
    pthread_t th;
    pthread_create(&th, NULL, watchdog_main, (void *)(uintptr_t)atoi(e));
    pthread_detach(th);
}

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    rt_unwind_point();
    KThread *me = t_cur;
    if (me) {
        me->svc_since = plat_now_ns();
        for (int i = 0; i < 4; i++) me->svc_args[i] = c->x[i];
        me->svc_lr = c->x[30];
        me->svc_now = imm + 1;
    }
#ifdef __ANDROID__
    kernel_preempt_point();
    bool blocks = svc_blocks(imm);
    if (blocks) core0_release(t_cur);
#endif
#if TOTK_DIAGNOSTICS
    uint64_t probe_body_begin=wait_probe_on()?plat_now_ns():0;
#endif
    svc_entry(c, imm, pc);
#if TOTK_DIAGNOSTICS
    if(probe_body_begin && imm<0x80)wait_probe.service_body_ns[imm]+=plat_now_ns()-probe_body_begin;
#endif
#ifdef __ANDROID__
    if (blocks) core0_acquire(t_cur);
    kernel_preempt_point();
#endif
#if TOTK_DIAGNOSTICS
    // Includes lock entry, cooperative handoff and nonpositive SleepThread
    // yields omitted by the narrower wait buckets. Do not add the two totals.
    if(wait_probe_on() && me && imm<0x80) {
        wait_probe.service_ns[imm]+=plat_now_ns()-me->svc_since;
        ++wait_probe.service_calls[imm];
        if(imm==0x0b)++wait_probe.yields[(int64_t)me->svc_args[0]<0?0:me->svc_args[0]?2:1];
    }
#endif
    if (me) me->svc_now = 0;
#if TOTK_DIAGNOSTICS
    wait_probe_long_flush();
    if(wait_probe_trace_pending) {
        uint64_t address=wait_probe_trace_pending;wait_probe_trace_pending=0;
        const char *at=diag_getenv("LARECOMP_KERNEL_WAIT_BT_FRAME");
        if(at && wait_probe_trace_count<4 && atomic_load_explicit(&wait_probe_frame,memory_order_relaxed)>=strtoull(at,NULL,10)) {
            bool seen=false;for(unsigned i=0;i<wait_probe_trace_count;++i)seen|=wait_probe_trace_addr[i]==address;
            if(!seen) {
                wait_probe_trace_addr[wait_probe_trace_count++]=address;
                LOG("[kernel-wait-bt] thread %llu address %llx caller %llx\n",(unsigned long long)(me?me->id:0),
                    (unsigned long long)address,(unsigned long long)(me?me->svc_lr:0));
                plat_print_backtrace();
            }
        }
    }
    // Follow the presenter to its producer using that producer's own TLS
    // counters. SVC locks are released here; no cross-thread counter reads.
    if(wait_probe_on() && me && (me->id==atomic_load_explicit(&wait_probe_signaler,memory_order_relaxed) ||
       me->id==atomic_load_explicit(&wait_probe_worker,memory_order_relaxed)) &&
       me->id!=atomic_load_explicit(&wait_probe_presenter,memory_order_relaxed))
        wait_probe_report(atomic_load_explicit(&wait_probe_frame,memory_order_relaxed));
#endif
    rt_unwind_point();
}

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc) {
    if (prof_svc < 0) prof_svc = DIAG_ENV("LARECOMP_PROF_SVC") != NULL;
    if (prof_svc && imm < 0x80) {
        uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], lr = c->x[30];
        uint32_t h0 = imm == 0x18 && a2 ? *(uint32_t *)(uintptr_t)a1 : 0;
        uint64_t t0 = plat_now_ns();
        svc_impl(c, imm, pc);
        uint64_t dt = plat_now_ns() - t0;
        static _Atomic int nslow;
        if (((t_cur && t_cur->id == 80 && (imm == 0x18 || imm == 0x34)) || (dt > 10000000 && imm != 0x0b && imm != 0x1c)) && nslow < 20000) {
            nslow++;
            LOG("[slow] t%llu svc%02x %llums a=%llx,%llx,%llx,%llx h0=%x lr=%llx\n", (unsigned long long)(t_cur ? t_cur->id : 0), imm,
                (unsigned long long)(dt / 1000000), (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, h0, (unsigned long long)lr);
        }
        unsigned tid = t_cur ? (unsigned)(t_cur->id & 31) : 31;
        svc_wait_ns[tid][imm] += dt;
        svc_calls[tid][imm]++;
        static _Atomic uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last > 2000000000ull) {
            last = now;
            for (int t = 0; t < 32; t++) {
                int any = 0;
                for (int i = 0; i < 0x80; i++)
                    if (svc_calls[t][i]) {
                        if (!any) { LOG("[svcprof] t%d", t); any = 1; }
                        LOG(" %02x:%llu/%llums", i, (unsigned long long)svc_calls[t][i], (unsigned long long)(svc_wait_ns[t][i] / 1000000));
                        svc_calls[t][i] = 0; svc_wait_ns[t][i] = 0;
                    }
                if (any) LOG("\n");
            }
        }
        return;
    }
    if (trace_svc < 0) trace_svc = diag_getenv("LARECOMP_TRACE_SVC") != NULL;
    if (!trace_svc) return svc_impl(c, imm, pc);
    uint64_t in[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
    svc_impl(c, imm, pc);
    LOG("[svc] %02x(%llx, %llx, %llx, %llx) -> %llx %llx %llx  @%llx\n", imm, in[0], in[1], in[2], in[3], c->x[0],
        c->x[1], c->x[2], pc);
}

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x01: {  // SetHeapSize
        uint64_t size = x[1];
        if (size > HEAP_MAX) { x[0] = R_OUT_OF_RESOURCE; break; }
        heap_size = size;
        region_remove(HEAP_BASE);
        if (size) region_add(HEAP_BASE, size, 5, 3);
#ifdef __ANDROID__
        {   // the heap as GPU memory where the driver allows it (nvn/vulkan/exec.c vk_heap_place), before the game writes it
            extern bool vk_heap_place(uint64_t size);
            LOG("[kernel] SetHeapSize %llu MB\n", (unsigned long long)(size >> 20));
            vk_heap_place(size);
        }
#endif
        x[0] = R_OK;
        x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;  // SetMemoryPermission / SetMemoryAttribute
    case 0x04:  // MapMemory(dst, src, size): src is inaccessible while mapped, so copying is equivalent
        // (an address outside the guest window, as a null stack from an allocation the game didn't check (issue #31),
        // is the kernel's InvalidAddress, not a copy from it)
        if (!guest_range_ok(x[0], x[2]) || !guest_range_ok(x[1], x[2])) {
            LOG("[kernel] MapMemory dst=%llx src=%llx size=%llx: outside the guest's memory\n",
                (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2]);
            x[0] = R_INVALID_ADDRESS;
            break;
        }
        memcpy((void *)x[0], (void *)x[1], x[2]);
        region_add(x[0], x[2], 0xB, 3);
        x[0] = R_OK;
        break;
    case 0x05:  // UnmapMemory(dst, src, size): copy back
        if (!guest_range_ok(x[0], x[2]) || !guest_range_ok(x[1], x[2])) {
            LOG("[kernel] UnmapMemory dst=%llx src=%llx size=%llx: outside the guest's memory\n",
                (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2]);
            x[0] = R_INVALID_ADDRESS;
            break;
        }
        memcpy((void *)x[1], (void *)x[0], x[2]);
        region_remove(x[0]);
        x[0] = R_OK;
        break;
    case 0x06:  // QueryMemory(out_info, _, addr)
        query_memory(x[2], x[0]);
        x[0] = R_OK;
        x[1] = 0;
        break;
    case 0x07: LOG("ExitProcess\n"); exit(0);
    case 0x08: {  // CreateThread(_, entry, arg, stack_top, prio, core)
        int core = (int32_t)x[5];
        if (core != -2 && (core < 0 || core > 3)) { x[0] = R_INVALID_CORE; break; }
        KThread *t = thread_new(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5]);
        t->handle = kernel_new_handle(&t->obj);
#ifdef __ANDROID__
        LOG("[kernel] CreateThread entry=%llx arg=%llx stack=%llx prio=%d core=%d -> %x (by %x)\n", x[1], x[2], x[3],
            (int)x[4], (int)(int32_t)x[5], t->handle, t_cur ? t_cur->handle : 0);
#else
        LOG("[kernel] CreateThread entry=%llx stack=%llx prio=%d -> %x\n", x[1], x[3], (int)x[4], t->handle);
#endif
        x[0] = R_OK;
        x[1] = t->handle;
        break;
    }
    case 0x09: {  // StartThread
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_THREAD) { x[0] = R_INVALID_HANDLE; break; }
        thread_start((KThread *)o);
        x[0] = R_OK;
        break;
    }
    case 0x0A: longjmp(t_cur->unwind, 1);  // ExitThread: back to thread_main, which signals the thread object
    case 0x0B: {  // SleepThread (in slices of at most 50 ms: a reboot doesn't wait out a long sleep)
        int64_t ns = (int64_t)x[0];
        if (ns <= 0) {
#ifdef __ANDROID__
            if(core0_guest_yield(ns))break;
#endif
            sched_yield();break;
        }
        for (uint64_t end = plat_now_ns() + (uint64_t)ns, now; !g_rt_unwind && (now = plat_now_ns()) < end;) {
            uint64_t d = end - now < 50000000 ? end - now : 50000000;
            struct timespec ts = {(time_t)(d / 1000000000), (long)(d % 1000000000)};
            nanosleep(&ts, NULL);
            uint64_t elapsed=plat_now_ns()-now;
            g_blocked_ns += elapsed;
            wait_probe_add(0x0b,elapsed);
        }
        break;
    }
    case 0x0C: {  // GetThreadPriority(_, handle)
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[1]);
        bool valid = o && o->type == KOBJ_THREAD;
        x[0] = valid ? R_OK : R_INVALID_HANDLE;
        x[1] = valid ? (uint64_t)thread_effective_priority((KThread *)o) : 0;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x0D: {  // SetThreadPriority
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        bool valid = o && o->type == KOBJ_THREAD;
        if (valid) thread_set_priority((KThread *)o, (int)x[1]);
        x[0] = valid ? R_OK : R_INVALID_HANDLE;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x0E: {  // GetThreadCoreMask(_, _, handle)
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[2]);
        bool valid = o && o->type == KOBJ_THREAD;
        x[0] = valid ? R_OK : R_INVALID_HANDLE;
        x[1] = valid ? (uint64_t)(int64_t)((KThread *)o)->ideal_core : 0;
        x[2] = valid ? ((KThread *)o)->core_mask : 0;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x0F: {  // SetThreadCoreMask(handle, ideal_core, affinity_mask)
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_THREAD) {
            pthread_mutex_unlock(&g_klock);
            x[0] = R_INVALID_HANDLE;
            break;
        }
        KThread *t = (KThread *)o;
        int old_core = __atomic_load_n(&t->core, __ATOMIC_ACQUIRE);
        uint32_t result = thread_set_core_mask(t, (int32_t)x[1], x[2]);
        if (DIAG_ENV("LARECOMP_LOG_AFFINITY"))
            LOG("[kernel] SetThreadCoreMask handle=%x core=%d mask=%llx by t%llu lr=%llx -> t%llu core %d->%d result=%x\n",
                (unsigned)x[0], (int)(int32_t)x[1], (unsigned long long)x[2],
                (unsigned long long)t_cur->id, (unsigned long long)c->x[30], (unsigned long long)t->id,
                old_core, __atomic_load_n(&t->core, __ATOMIC_ACQUIRE), result);
        pthread_mutex_unlock(&g_klock);
        x[0] = result;
        break;
    }
    case 0x10: x[0] = __atomic_load_n(&t_cur->core, __ATOMIC_ACQUIRE); break;  // GetCurrentProcessorNumber
    case 0x11: case 0x12: case 0x17: {  // SignalEvent / ClearEvent / ResetSignal
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o) x[0] = R_INVALID_HANDLE;
        else {
            KObject *ev = o->type == KOBJ_EVENT ? o : NULL;
            if (!ev) x[0] = R_INVALID_HANDLE;
            else {
                if (imm == 0x11) kernel_signal(ev);
                else if (imm == 0x17 && !ev->signaled) { x[0] = R_INVALID_STATE; pthread_mutex_unlock(&g_klock); break; }
                else ev->signaled = false;
                x[0] = R_OK;
            }
        }
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_SHMEM) { x[0] = R_INVALID_HANDLE; break; }
        KSharedMem *s = (KSharedMem *)o;
        memset((void *)x[1], 0, x[2]);
        s->guest_addr = x[1];  // HLE services write straight into the guest's mapping
        region_add(x[1], x[2], 6, 3);
        if (s->on_map) s->on_map(s);
        x[0] = R_OK;
        break;
    }
    case 0x14: region_remove(x[1]); x[0] = R_OK; break;  // UnmapSharedMemory
    case 0x15: {  // CreateTransferMemory(_, addr, size, perm)
        KTransferMem *t = calloc(1, sizeof *t);
        t->obj.type = KOBJ_TMEM;
        t->addr = x[1];
        t->size = x[2];
        x[0] = R_OK;
        x[1] = kernel_new_handle(&t->obj);
        break;
    }
    case 0x16:  // CloseHandle
        pthread_mutex_lock(&g_klock);
        handle_close((uint32_t)x[0]);
        pthread_mutex_unlock(&g_klock);
        x[0] = R_OK;
        break;
    case 0x18: {  // WaitSynchronization(_, handles, count, timeout)
        int32_t idx = -1;
        x[0] = svc_wait_synchronization(x[1], (int32_t)x[2], (int64_t)x[3], &idx);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {  // CancelSynchronization
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (o && o->type == KOBJ_THREAD) {
            ((KThread *)o)->cancel = true;
            wake((KThread *)o);
            x[0] = R_OK;
        } else x[0] = R_INVALID_HANDLE;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x1A: x[0] = svc_arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); break;
    case 0x1B: x[0] = svc_arbitrate_unlock(x[0]); break;
    case 0x1C: x[0] = svc_wait_process_wide_key(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]); break;
    case 0x1D: svc_signal_process_wide_key(x[0], (int32_t)x[1]); x[0] = R_OK; break;
    case 0x1E: x[0] = rt_ticks(); break;
    case 0x1F: {  // ConnectToNamedPort(_, name)
        uint32_t h = ipc_connect_named_port((const char *)(uintptr_t)x[1]);
        x[0] = h ? R_OK : R_NOT_FOUND;
        x[1] = h;
        break;
    }
    case 0x21: x[0] = ipc_handle_request(c, (uint32_t)x[0]); break;  // SendSyncRequest
    case 0x22: {  // SendSyncRequestWithUserBuffer(buf, size, handle)
        uint64_t saved = c->tpidrro;
        c->tpidrro = x[0];
        x[0] = ipc_handle_request(c, (uint32_t)x[2]);
        c->tpidrro = saved;
        break;
    }
    case 0x24: x[0] = R_OK; x[1] = 0x81; break;  // GetProcessId
    case 0x25: {  // GetThreadId(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? ((KThread *)o)->id : 0;
        break;
    }
    case 0x26:  // Break
        if (x[0] & 0x80000000u) { LOG("[kernel] Break notification %llx\n", x[0]); x[0] = R_OK; break; }
        {   // what failed: the 4-byte info is the Result an abort carries; the callers by the frame pointers
            if (x[2] == 4 && x[1] >= GUEST_LO && x[1] + 4 <= GUEST_HI)
                LOG("[kernel] svcBreak: result %#x (module %u, description %u)\n", *u32p(x[1]), *u32p(x[1]) & 0x1FF,
                    (*u32p(x[1]) >> 9) & 0x1FFF);
            LOG("[kernel] svcBreak: lr %llx, callers", (unsigned long long)x[30]);
            uint64_t fp = x[29];
            for (int i = 0; i < 12 && fp >= GUEST_LO && fp + 16 <= GUEST_HI; i++, fp = *(uint64_t *)(uintptr_t)fp)
                LOG(" %llx", (unsigned long long)*(uint64_t *)(uintptr_t)(fp + 8));
            LOG("\n");
        }
        FATAL("svcBreak(reason=%llx, info=%llx, size=%llx) at %llx\n", x[0], x[1], x[2], pc);
    case 0x27: debug_string(x[0], x[1]); x[0] = R_OK; break;
    case 0x29: {  // GetInfo(_, id0, handle, id1)
        uint32_t id0 = (uint32_t)x[1];
        uint64_t v = 0;
        uint32_t r = R_OK;
        switch (id0) {
        case 0: {  // core mask (the NPDM says cores 0-2, 0x7; 0x7 changed none of BOTW's heap budgets).
                   // LARECOMP_CORE_MASK=<hex> (test)
            static uint64_t m;
            if (!m) m = getenv("LARECOMP_CORE_MASK") ? strtoull(getenv("LARECOMP_CORE_MASK"), NULL, 16) : 0xF;
            v = m;
            break;
        }
        case 1: v = 0xFFFFFFFFFFFFFFFFull; break;
        case 2: v = ALIAS_BASE; break;
        case 3: v = ALIAS_SIZE; break;
        case 4: v = HEAP_BASE; break;
        case 5: v = HEAP_MAX; break;
        case 6: case 21: {  // application memory (3.2 GB, as on hardware); LARECOMP_TOTAL_MEM=<hex bytes> (test)
            v = memory_total() - (id0 == 21 ? system_resource_size : 0);
            static int nlog;
            if (nlog++ < 8) LOG("[kernel] GetInfo total memory -> %#llx\n", (unsigned long long)v);
            break;
        }
        case 7: case 22: {  // used: the heap + the rest (code, data, stacks); LARECOMP_USED_EXTRA=<hex bytes> (test)
            v = heap_size + memory_extra() + physical_used() + (id0 == 7 ? system_resource_size : 0);
            static int nlog;
            if (nlog++ < 8) LOG("[kernel] GetInfo used memory -> %#llx (heap %#llx)\n", (unsigned long long)v, (unsigned long long)heap_size);
            break;
        }
        case 8: v = 0; break;
        case 9: v = 0; break;
        case 11: v = ((uint64_t)arc4random() << 32) | arc4random(); break;
        case 12: v = GUEST_LO; break;
        case 13: v = GUEST_HI - GUEST_LO; break;
        case 14: v = STACK_BASE; break;
        case 15: v = STACK_SIZE; break;
        case 16: v = system_resource_size; break;
        case 17: v = 0; break;  // host manages page-table storage
        case 18: v = TOTK_TITLE_ID; break;  // program id (BOTW)
        // UserExceptionContextAddress: a zeroed page at the top of the TLS region (BOTW 1.8.2's SDK passes it to
        // SetDyingMessageRegion, which needs it non-null - SavageWindRecomp); AliasRegionExtraSize (HOS 18+): none
        case 20: v = TLS_BASE + TLS_SIZE - 0x1000; break;
        case 28: v = 0; break;
        default: r = R_INVALID_ENUM; LOG("[kernel] GetInfo unknown id %u/%llu\n", id0, x[3]);
        }
        x[0] = r;
        x[1] = v;
        break;
    }
    case 0x2C: x[0] = physical_memory(x[0], x[1], true); break;
    case 0x2D: x[0] = physical_memory(x[0], x[1], false); break;
    case 0x34: x[0] = svc_wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]); break;
    case 0x35: x[0] = svc_signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); break;
    case 0x45: {  // CreateEvent -> w1 writable, w2 readable
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        x[0] = R_OK;
        x[1] = kernel_new_handle(ev);
        x[2] = kernel_new_handle(ev);
        if (DIAG_ENV("LARECOMP_PROF_SVC")) LOG("[evt] guest w=%llx r=%llx lr=%llx\n", (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[30]);
        break;
    }
    default:
        FATAL("unimplemented svc 0x%x at %llx (x0=%llx x1=%llx x2=%llx)\n", imm, pc, x[0], x[1], x[2]);
    }
}

// Reboot, step 1: stop every guest thread (see rt_unwind_point) and join it. `idle` keeps the window responsive.
void kernel_stop_threads(void (*idle)(void)) {
    g_rt_unwind = 1;
    uint64_t t0 = plat_now_ns(), logged = t0;
    for (;;) {
        int live = 0;
        pthread_mutex_lock(&g_klock);
        for (KThread *t = g_threads; t; t = t->anext)
            if (t->started && !t->finished) { live++; wake(t); }
        pthread_cond_broadcast(&g_nothread_cond);
        pthread_mutex_unlock(&g_klock);
#ifdef __ANDROID__
        pthread_mutex_lock(&c0_m);
        pthread_cond_broadcast(&c0_cv);
        pthread_mutex_unlock(&c0_m);
#endif
        if (!live) break;
        uint64_t now = plat_now_ns();
        if (now - logged > 3000000000ull) {  // (a thread that never reaches a safe point: who and where)
            logged = now;
            LOG("[reboot] still waiting for %d guest threads after %llums:\n", live, (unsigned long long)((now - t0) / 1000000));
            for (KThread *t = g_threads; t; t = t->anext)
                if (t->started && !t->finished)
                    LOG("[reboot]   thread %llu (entry %llx) pc %llx lr %llx wait %d sync %d\n", (unsigned long long)t->id,
                        (unsigned long long)t->entry, (unsigned long long)t->cpu.pc, (unsigned long long)t->cpu.x[30],
                        t->wait_kind, t->in_sync_wait);
        }
        if (idle) idle();
        usleep(2000);
    }
    int n = 0;
    for (KThread *t = g_threads; t; t = t->anext)
        if (t->started) { pthread_join(t->host, NULL); n++; }
    LOG("[reboot] %d guest threads stopped in %llums\n", n, (unsigned long long)((plat_now_ns() - t0) / 1000000));
}

// Reboot, step 2 (threads stopped): a fresh kernel. Kernel objects are leaked rather than freed: host statics
// can still point at some (vi.c's vsync event, signalled by its host timer thread).
void kernel_reset(void) {
    pthread_mutex_lock(&g_klock);
#if TOTK_DIAGNOSTICS
    atomic_store(&wait_probe_presenter,0);atomic_store(&wait_probe_signaler,0);
    atomic_store(&wait_probe_signal_lr,0);atomic_store(&wait_probe_frame,0);
    atomic_store(&wait_probe_worker,0);
    memset(wait_probe_wakers,0,sizeof wait_probe_wakers);
    // Other guest threads have been joined; fresh threads start with fresh TLS.
    memset(&wait_probe_long,0,sizeof wait_probe_long);
#endif
#ifdef __ANDROID__
    pthread_mutex_lock(&c0_m);
    c0_head = c0_tail = c0_holder = NULL;
    c0_priority_preempted = NULL;
    free(c0_priorities);c0_priorities = NULL;c0_priority_capacity = 0;c0_mutex_waiters = 0;
    c0_nevicted = 0;
#if TOTK_DIAGNOSTICS
    c0_hold_reports=0;
#endif
    __atomic_fetch_and(&g_fnprof, ~2, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&c0_m);
#endif
    for (KThread *t = g_threads, *next; t; t = next) {
        next = t->anext;
        pthread_cond_destroy(&t->cond);
#ifdef __ANDROID__
        free(t->dcache);
#endif
        free(t);
    }
    g_threads = NULL;
    g_waitlist = NULL;
    g_syncwaiters = NULL;
    memset(handles, 0, sizeof handles);
    nregions = 0;
    heap_size = 0;
    tls_next = TLS_BASE;
    next_thread_id = 0x50;
    g_rt_unwind = 0;
    pthread_mutex_unlock(&g_klock);
}

void kernel_init(void) {
    watchdog_start();
#ifdef __ANDROID__
    tls_next = TLS_BASE;
#endif
    // Reserve the heap, alias, stack, and TLS regions.
    region_add(TLS_BASE, TLS_SIZE, 0xC, 3);
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        region_add(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        region_add(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        region_add(m->base + m->data_off, m->size - m->data_off, 4, 3);
    }
}

KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    KObject *ev = calloc(1, sizeof *ev);
    ev->type = KOBJ_EVENT;
    *w = kernel_new_handle(ev);
    *r = kernel_new_handle(ev);
    if (DIAG_ENV("LARECOMP_PROF_SVC")) { const char *s = plat_symbol((uint64_t)(uintptr_t)__builtin_return_address(0), NULL); LOG("[evt] hle w=%x r=%x from %s\n", *w, *r, s ? s : "?"); }
    return ev;
}

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) {
    KSharedMem *s = calloc(1, sizeof *s);
    s->obj.type = KOBJ_SHMEM;
    s->size = size;
    s->on_map = on_map;
    *handle = kernel_new_handle(&s->obj);
    return s;
}
