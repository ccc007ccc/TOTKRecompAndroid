// Exercise the actual kernel's physical mapping path without proprietary inputs.
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/kernel.c"

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    assert(lo >= ALIAS_BASE && hi <= ALIAS_BASE + 0x100000);
    return mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)lo;
}
void rt_fatal_trace(void) { abort(); }

static uint64_t type_at(uint64_t addr) {
    uint64_t info[5];
    query_memory(addr, (uint64_t)(uintptr_t)info);
    return info[2];
}

int main(void) {
    uint64_t base = ALIAS_BASE;
    void *area = mmap((void *)base, 0x100000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(area == (void *)base);
    memset(area, 0xcc, 0x100000);
    assert(physical_memory(base, 0x1000, true) == R_INVALID_STATE);
    kernel_set_system_resource_size(0x1800000);
    assert(physical_memory(base + 1, 0x1000, true) == R_INVALID_ADDRESS);
    assert(physical_memory(base, 0, true) == R_INVALID_ADDRESS);
    assert(physical_memory(base + ALIAS_SIZE, 0x1000, true) == R_INVALID_ADDRESS);

    assert(!physical_memory(base + 0x1000, 0x2000, true));
    assert(physical_used() == 0x2000 && type_at(base + 0x1000) == 5);
    *(uint32_t *)(base + 0x1040) = 0x12345678;
    assert(!physical_memory(base, 0x5000, true));
    assert(*(uint32_t *)(base + 0x1040) == 0x12345678);  // overlapping map preserves data
    assert(physical_used() == 0x5000 && nregions == 1);
    assert(!physical_memory(base + 0x2000, 0x1000, false));
    assert(type_at(base + 0x2000) == 0 && physical_used() == 0x4000 && nregions == 2);
    assert(!physical_memory(base + 0x2000, 0x1000, true));
    assert(*(uint32_t *)(base + 0x2000) == 0 && *(uint32_t *)(base + 0x1040) == 0x12345678);
    assert(nregions == 1 && physical_used() == 0x5000);

    region_add(base + 0x8000, 0x1000, 6, 3);
    assert(physical_memory(base + 0x7000, 0x3000, true) == R_INVALID_STATE);
    assert(type_at(base + 0x7000) == 0 && type_at(base + 0x8000) == 6);
    assert(physical_memory(base + 0x7000, 0x3000, false) == R_INVALID_STATE);
    assert(!physical_memory(base, 0x5000, false));
    assert(physical_used() == 0 && type_at(base + 0x8000) == 6);

    assert(!physical_memory(base + 0x10000, 0x10000, true));
    memset((void *)(base + 0x10000), 0x55, 0x10000);
    assert(!physical_memory(base + 0x14000, 0x4000, false));
    assert(*(uint8_t *)(base + 0x13000) == 0x55 && *(uint8_t *)(base + 0x18000) == 0x55);
    assert(!physical_memory(base + 0x14000, 0x4000, true));
    assert(*(uint8_t *)(base + 0x14000) == 0 && physical_used() == 0x10000);
    setenv("LARECOMP_TOTAL_MEM", "4000000", 1);
    assert(physical_memory(base + 0x20000, 0x1000, true) == R_OUT_OF_RESOURCE);
    assert(type_at(base + 0x20000) == 0);
    unsetenv("LARECOMP_TOTAL_MEM");
    munmap(area, 0x100000);
    puts("physical memory tests passed: overlap, split, remap, isolation and budget");
}
