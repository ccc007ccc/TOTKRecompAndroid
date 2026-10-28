#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define SHADOW_PAGE 0x10000ull
#define SHADOW_K 8
typedef struct { uint64_t words[8]; } CmdRec;
#define FATAL(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
static _Atomic unsigned locks;
static int tracked_lock(pthread_mutex_t *m) {
    atomic_fetch_add_explicit(&locks, 1, memory_order_relaxed);
    return pthread_mutex_lock(m);
}
static _Atomic uint64_t frame;
uint64_t renderer_present_count(void) { return atomic_load_explicit(&frame, memory_order_relaxed); }
#define pthread_mutex_lock tracked_lock
#include "../runtime/nvn/command_shadow.h"
#undef pthread_mutex_lock

enum { THREADS = 8, PAGES = 64, REPEATS = 60000 };
static pthread_barrier_t start;
static uint64_t seen[THREADS][PAGES];
static const uint64_t guest_base = 0x240000000ull;
static void *worker(void *arg) {
    unsigned t = (unsigned)(uintptr_t)arg;
    shadow_init();
    pthread_barrier_wait(&start);
    for (unsigned i = 0; i < REPEATS; i++) {
        unsigned p = (i * 17 + t * 7) % PAGES;
        uint64_t offset = (i * 31) % SHADOW_PAGE;
        uint64_t base = shadow_at(guest_base + p * SHADOW_PAGE + offset) - offset * SHADOW_K;
        if (seen[t][p]) assert(seen[t][p] == base);
        seen[t][p] = base;
    }
    return NULL;
}
static unsigned slot(uint64_t page) {
    return (unsigned)(page * 0x9E3779B97F4A7C15ull >> 48) & (SHADOW_SLOTS - 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    bool locked = !strcmp(argv[1], "locked"), ab = !strcmp(argv[1], "ab");
    setenv("LARECOMP_SHADOW_LOCKED", locked ? "1" : "0", 1);
    if (ab) setenv("LARECOMP_AB", "1:shadow", 1);
    else unsetenv("LARECOMP_AB");
    if (ab) atomic_store(&frame, 1);  // fast phase
    shadow_init();
    uint64_t zero = shadow_at(0);  // page zero also has a valid nonzero key
    assert(shadow_at(7) == zero + 7 * SHADOW_K);
    uint64_t a = shadow_at(guest_base);
    *(uint64_t *)(uintptr_t)a = 0x123456789abcdef0ull;
    unsigned before = atomic_load(&locks);
    for (int i = 0; i < 10000; i++) assert(shadow_at(guest_base + 19) == a + 19 * SHADOW_K);
    assert(atomic_load(&locks) - before == (locked ? 10000u : 0u));
    assert(*(uint64_t *)(uintptr_t)a == 0x123456789abcdef0ull);
    assert(shadow_at(guest_base + SHADOW_PAGE - 1) == a + (SHADOW_PAGE - 1) * SHADOW_K);
    if (ab) {
        atomic_store(&frame, 0); before = atomic_load(&locks);
        assert(shadow_at(guest_base) == a && atomic_load(&locks) == before + 1);
        atomic_store(&frame, 1);
    }
    // Force an open-addressing collision, then verify both permanent mappings remain stable.
    uint64_t first = guest_base / SHADOW_PAGE + 1, other = first + 1;
    while (slot(other) != slot(first)) other++;
    uint64_t b = shadow_at((other - 1) * SHADOW_PAGE);
    assert(b != a && shadow_at(guest_base) == a);
    assert(shadow_at((other - 1) * SHADOW_PAGE + 13) == b + 13 * SHADOW_K);
    pthread_t threads[THREADS];
    assert(!pthread_barrier_init(&start, NULL, THREADS));
    for (unsigned t = 0; t < THREADS; t++) assert(!pthread_create(&threads[t], NULL, worker, (void *)(uintptr_t)t));
    for (unsigned t = 0; t < THREADS; t++) assert(!pthread_join(threads[t], NULL));
    for (unsigned p = 0; p < PAGES; p++)
        for (unsigned t = 1; t < THREADS; t++) assert(seen[t][p] && seen[t][p] == seen[0][p]);
    pthread_barrier_destroy(&start);
    printf("Command shadow %s: collisions, offsets, immutable mappings and 480000 concurrent lookups OK\n", argv[1]);
}
