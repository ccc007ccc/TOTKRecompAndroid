// Paired atomics used by TotK's 16-byte atomic load/store implementations.
#include "cpu.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static __uint128_t value __attribute__((aligned(16)));
static uint64_t address(void) { return (uint64_t)(uintptr_t)&value; }

static void *increment(void *unused) {
    (void)unused;
    Cpu c = {0};
    for (int i = 0; i < 10000; i++) {
        for (;;) {
            __uint128_t pair = ldxp64(&c, address());
            uint64_t lo = (uint64_t)pair, hi = (uint64_t)(pair >> 64);
            assert(hi == ~lo);  // a torn pair must never be observed
            if (!stxp64(&c, address(), lo + 1, ~(lo + 1))) break;
        }
    }
    return NULL;
}

int main(void) {
    Cpu a = {0}, b = {0};
    value = ((__uint128_t)2 << 64) | 1;
    assert(ldxp64(&a, address()) == value);
    assert(stxp64(&a, address(), 3, 4) == 0);
    assert(value == (((__uint128_t)4 << 64) | 3));
    assert(stxp64(&a, address(), 5, 6) == 1);  // reservation consumed

    ldxp64(&a, address());
    ldxp64(&b, address());
    assert(stxp64(&b, address(), 7, 8) == 0);
    assert(stxp64(&a, address(), 9, 10) == 1);
    assert(value == (((__uint128_t)8 << 64) | 7));

    ldxp64(&a, address());
    assert(stx64(&a, address(), 0) == 1);  // width mismatch also consumes reservation
    assert(stxp64(&a, address(), 0, 0) == 1);
    ldxp64(&a, address());
    assert(stxp64(&a, address() + 16, 0, 0) == 1);
    assert(stxp64(&a, address(), 0, 0) == 1);
    ldxp64(&a, address());
    a.excl_addr = ~0ull;  // CLREX
    assert(stxp64(&a, address(), 0, 0) == 1);

    uint64_t small = 0xabcdef0198765432ull;
    assert(ldxp32(&a, (uint64_t)(uintptr_t)&small) == small);
    assert(!stxp32(&a, (uint64_t)(uintptr_t)&small, 0x100000001ull, 0x100000002ull));
    assert(small == 0x0000000200000001ull);

    value = (__uint128_t)~0ull << 64;
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, increment, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert((uint64_t)value == 40000 && (uint64_t)(value >> 64) == ~40000ull);
    puts("paired exclusive tests passed (40,000 concurrent updates)");
}
