// Command shadow pages are allocated once and never removed, including across game resets. Publish each
// pointer with its page key: readers that acquire that key may use the immutable pointer without a mutex.
#pragma once
#include <stdatomic.h>

enum { SHADOW_SLOTS = 1 << 16 };
static struct { _Atomic uint64_t page; uint8_t *mem; } g_shadow[SHADOW_SLOTS];
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_shadow_once = PTHREAD_ONCE_INIT;
static bool g_shadow_fast = true;
#ifdef __ANDROID__
static uint64_t g_shadow_ab;
#endif

static void shadow_configure(void) {
    const char *locked = getenv("LARECOMP_SHADOW_LOCKED");
    g_shadow_fast = !locked || atoi(locked) == 0;
#ifdef __ANDROID__
    const char *ab = getenv("LARECOMP_AB"), *colon = ab ? strchr(ab, ':') : NULL;
    long frames = ab ? atol(ab) : 0;
    if (frames > 0 && colon && !strcmp(colon + 1, "shadow")) g_shadow_ab = (uint64_t)frames;
#endif
}

// Called when a command buffer is initialized, before any thread records into it. Keeping pthread_once out of
// shadow_at avoids replacing one per-command synchronization cost with another.
static void shadow_init(void) { pthread_once(&g_shadow_once, shadow_configure); }

static uint64_t shadow_at(uint64_t guest) {
    uint64_t page = guest / SHADOW_PAGE + 1;
    uint32_t first = (uint32_t)(page * 0x9E3779B97F4A7C15ull >> 48) & (SHADOW_SLOTS - 1);
    bool fast = g_shadow_fast;
#ifdef __ANDROID__
    if (g_shadow_ab) {
        extern uint64_t renderer_present_count(void);
        fast = (renderer_present_count() / g_shadow_ab) & 1;
    }
#endif
    if (fast) {
        uint32_t i = first;
        for (;;) {
            uint64_t found = atomic_load_explicit(&g_shadow[i].page, memory_order_acquire);
            if (found == page)
                return (uint64_t)(uintptr_t)g_shadow[i].mem + guest % SHADOW_PAGE * SHADOW_K;
            if (!found) break;
            i = (i + 1) & (SHADOW_SLOTS - 1);
        }
    }
    pthread_mutex_lock(&g_shadow_lock);
    uint32_t i = first;
    uint64_t found;
    while ((found = atomic_load_explicit(&g_shadow[i].page, memory_order_relaxed)) && found != page)
        i = (i + 1) & (SHADOW_SLOTS - 1);
    if (!found) {
        uint8_t *mem = malloc(SHADOW_PAGE * SHADOW_K + sizeof(CmdRec));
        if (!mem) FATAL("nvn: no memory for a command shadow page\n");
        g_shadow[i].mem = mem;
        atomic_store_explicit(&g_shadow[i].page, page, memory_order_release);
    }
    uint8_t *mem = g_shadow[i].mem;
    pthread_mutex_unlock(&g_shadow_lock);
    return (uint64_t)(uintptr_t)mem + guest % SHADOW_PAGE * SHADOW_K;
}
