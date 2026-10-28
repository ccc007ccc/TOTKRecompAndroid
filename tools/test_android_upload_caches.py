#!/usr/bin/env python3
"""Execute the actual C upload-cache helpers on the PC via WebAssembly (no device).
Requires Python, Node.js and the Android NDK's clang/ld.lld. Pass its bin directory as argv[1].
The allocator, submission counter and arena allocator are test doubles; no Vulkan calls are executed.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'optimization-tests'
OUT.mkdir(parents=True, exist_ok=True)
BIN = pathlib.Path(sys.argv[1])
EXE = '.exe' if sys.platform == 'win32' else ''
textures = (ROOT / 'runtime/nvn/vulkan/textures.c').read_text()
execution = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
texture_helpers = textures[textures.index('static uint8_t *upload_scratch;'):textures.index('uint64_t prof_tex_checks,')]
arena_helper = execution[execution.index('static uint64_t ga_load_copy('):execution.index('static bool gres_prepare(')]
heap_fallback = execution[execution.index('bool vk_heap_place('):execution.index('#endif', execution.index('bool vk_heap_place('))]
ubo_helper = execution[execution.index('static bool is_ubo('):execution.index('\n', execution.index('static bool is_ubo('))]
plan_start = execution.index('typedef struct { uint8_t regular[MAX_BIND]')
descriptor_helpers = execution[plan_start:execution.index('\n#endif', plan_start)]
copy_start = execution.index('// Guest memory copied once per submission:')
copy_helpers = execution[copy_start:execution.index('// ================================================================== worker threads', copy_start)]
preamble = r"""
typedef unsigned long long uint64_t;
typedef unsigned int size_t;
typedef unsigned int uintptr_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define MAX_BIND 96
#define GMAP_BUFFER_INDEX 20
typedef enum {
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
} VkDescriptorType;
typedef struct { uint32_t set, binding; VkDescriptorType type; } VkBindingInfo;
static unsigned char heap[2 << 20];
static unsigned used;
static int fail_alloc;
void *memcpy(void *to, const void *from, size_t size) {
    for (unsigned i = 0; i < size; i++) ((uint8_t *)to)[i] = ((const uint8_t *)from)[i];
    return to;
}
int memcmp(const void *a, const void *b, size_t size) {
    for (unsigned i = 0; i < size; i++) if (((const uint8_t *)a)[i] != ((const uint8_t *)b)[i]) return 1;
    return 0;
}
void *realloc(void *old, size_t n) {
    if (fail_alloc || used + n + 16 > sizeof heap) return NULL;
    unsigned *header = (unsigned *)(heap + used);
    *header = n;
    void *next = header + 1;
    if (old) memcpy(next, old, ((unsigned *)old)[-1] < n ? ((unsigned *)old)[-1] : n);
    used = (used + n + 19) & ~15u;
    return next;
}
void abort(void) { __builtin_trap(); }
char *getenv(const char *name) { return NULL; }
int atoi(const char *s) { return 0; }
typedef struct { uint64_t data_hash; } TexBackend;
static uint8_t guest[65536], arena[4096];
#define HEAP_BASE ((uintptr_t)guest)
#define HEAP_MAX sizeof guest
static bool gcopy_mode, place_result;
static int tracking_calls;
static uint64_t tracking_lo, tracking_hi;
static bool try_heap_place(uint64_t size) { return place_result; }
static void plat_write_tracking_enable(uint64_t lo, uint64_t hi) {
    tracking_calls++;
    tracking_lo = lo; tracking_hi = hi;
}
static uint8_t *ga_ptr = arena;
static uint64_t gcopy_gen, seq = 1, top;
static uint64_t vk_pending_seq(void) { return seq; }
static uint64_t ga_ring(uint64_t n) {
    if (top + n > sizeof arena) return ~0ull;
    uint64_t off = top; top += n; return off;
}
typedef struct { uint8_t *ptr; } VkSpan;
static uint8_t snapshots[1 << 18];
static unsigned snapshot_used, snapshot_copies;
static uint64_t stat_cb_copied, stat_cb_hits, stat_cb_misses, stat_cb_collisions;
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    if (snapshot_used + size > sizeof snapshots) abort();
    VkSpan s = {snapshots + snapshot_used};
    memcpy(s.ptr, data, size);
    snapshot_used += size;
    snapshot_copies++;
    return s;
}

"""
tests = r"""
#define CHECK(c) do { count++; if (!(c)) return count; } while (0)
int test(void) {
    int count = 0;
    // Layout creation converts UBOs to dynamic UBOs before building these plans. Neither kind may disappear.
    VkBindingInfo binds[] = {
        {0, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {1, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {0, 18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {0, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        {0, 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        {0, 19, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
        {0, GMAP_BUFFER_INDEX, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
        {0, 17, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {0, 32, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    };
    DescriptorPlan plan;
    descriptor_plan(binds, sizeof binds / sizeof binds[0], 0, &plan);
    CHECK(plan.nregular == 4 && plan.ndeferred == 3);
    CHECK(plan.regular[0] == 0 && plan.regular[1] == 3 && plan.regular[2] == 4 && plan.regular[3] == 7);
    CHECK(plan.deferred[0] == 2 && plan.deferred[1] == 5 && plan.deferred[2] == 6);
    descriptor_plan(binds, sizeof binds / sizeof binds[0], 1, &plan);
    CHECK(plan.nregular == 1 && plan.regular[0] == 1 && plan.ndeferred == 0);
    descriptor_plan(binds, 0, 0, &plan);
    CHECK(plan.nregular == 0 && plan.ndeferred == 0);
    VkBindingInfo full[MAX_BIND];
    for (int i = 0; i < MAX_BIND; i++) full[i] = (VkBindingInfo){0, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
    descriptor_plan(full, MAX_BIND, 0, &plan);
    CHECK(plan.nregular == MAX_BIND && plan.regular[MAX_BIND - 1] == MAX_BIND - 1);

    // Distinct guest ranges with the same direct-mapped slot must remain independent snapshots.
    uint64_t keys[5], wanted = 0;
    unsigned found = 0;
    for (unsigned i = 0; i + 16 <= sizeof guest && found < 5; i++) {
        uint64_t addr = (uintptr_t)(guest + i);
        uint64_t hash = (addr * 0x9E3779B97F4A7C15ull) ^ (16ull * 0xC2B2AE3D27D4EB4Full);
        uint64_t slot = (uint32_t)(hash ^ (hash >> 32)) & (GCOPY_TAB - 1);
        if (!found) wanted = slot;
        if (slot == wanted) keys[found++] = addr;
    }
    CHECK(found == 5);
    seq = 100;
    VkSpan first = guest_copy(keys[0], 16);
    CHECK(guest_copy(keys[0], 16).ptr == first.ptr && snapshot_copies == 1);
    guest_copy(keys[1], 16);
    CHECK(guest_copy(keys[0], 16).ptr != first.ptr && snapshot_copies == 3);
    CHECK(gcopy_nactive == 1);
    // Repeated inline updates must not exhaust registration and leave a later copy untracked.
    for (int i = 0; i < GCOPY_TAB + 8; i++) {
        guest_copy_invalidate(keys[0], keys[0] + 1);
        guest_copy(keys[0], 16);
    }
    CHECK(gcopy_nactive == 1);
    VkSpan later = guest_copy((uintptr_t)(guest + 100), 16);
    guest[100] = 77;
    guest_copy_invalidate((uintptr_t)(guest + 100), (uintptr_t)(guest + 101));
    VkSpan changed = guest_copy((uintptr_t)(guest + 100), 16);
    CHECK(changed.ptr != later.ptr && changed.ptr[0] == 77 && later.ptr[0] == 0);
    seq++;
    VkSpan new_submission = guest_copy((uintptr_t)(guest + 100), 16);
    CHECK(new_submission.ptr != changed.ptr && new_submission.ptr[0] == 77);
    guest_copy_flush();
    CHECK(guest_copy((uintptr_t)(guest + 100), 16).ptr != new_submission.ptr);
    // An advertised placement extension can still be disabled or fail. Copy-mode caches must remain available.
    gcopy_mode = true;
    CHECK(!vk_heap_place(1024) && tracking_calls == 1);
    CHECK(tracking_lo == HEAP_BASE && tracking_hi == HEAP_BASE + HEAP_MAX);
    // Direct GPU mappings must never receive CPU page protection through this fallback.
    place_result = true;
    CHECK(vk_heap_place(1024) && tracking_calls == 1);
    place_result = false; gcopy_mode = false;
    CHECK(!vk_heap_place(1024) && tracking_calls == 1);
    gcopy_mode = true;
    CHECK(!vk_heap_place(0) && tracking_calls == 1);
    TexBackend b = {123};
    uint8_t pixels[64] = {1, 2, 3};
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    CHECK(texture_unchanged(&b, pixels, sizeof pixels));
    pixels[37] = 9;
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    b.data_hash++;
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    CHECK(texture_unchanged(&b, pixels, sizeof pixels));
    texture_snapshot_forget(&b);
    CHECK(!texture_unchanged(&b, pixels, sizeof pixels));
    CHECK(!texture_unchanged(&b, pixels, (1u << 20) + 1));
    CHECK(!texture_unchanged(&b, pixels, 0));
    fail_alloc = 1;
    CHECK(!texture_unchanged(&b, pixels, 32));
    fail_alloc = 0;
    CHECK(texture_unchanged(&b, pixels, sizeof pixels));
    uint8_t *scratch = texture_scratch(64);
    scratch[13] = 42;
    CHECK(texture_scratch(16) == scratch);
    CHECK(texture_scratch(256)[13] == 42);
    CHECK(texture_scratch(128) == upload_scratch);

    guest[0] = 1;
    uint64_t a = ga_load_copy(0, 64);
    CHECK(a == 0 && top == 64 && arena[a] == 1);
    CHECK(ga_load_copy(0, 64) == a && top == 64);
    guest[63] = 7;
    uint64_t bcopy = ga_load_copy(0, 64);
    CHECK(bcopy != a && arena[bcopy + 63] == 7 && arena[a + 63] == 0);
    seq++;
    uint64_t c = ga_load_copy(0, 64);
    CHECK(c != bcopy);
    gcopy_gen++;
    uint64_t d = ga_load_copy(0, 64);
    CHECK(d != c);
    CHECK(ga_load_copy(0, 64) == d);
    top = sizeof arena;
    guest[20]++;
    CHECK(ga_load_copy(0, 64) == ~0ull);
    CHECK(arena[d + 20] == 0);
    guest[20]--;
    CHECK(ga_load_copy(0, 64) == d);
    return 0;
}
"""
source = OUT / 'upload_caches.c'
source.write_text(preamble + ubo_helper + descriptor_helpers + texture_helpers + arena_helper + heap_fallback + copy_helpers + tests)
subprocess.run([str(BIN / ('clang' + EXE)), '-target', 'wasm32', '-D__ANDROID__', '-ffreestanding', '-fno-builtin', '-O2', '-c', str(source), '-o', str(OUT / 'upload_caches.o')], check=True)
subprocess.run([str(BIN / ('ld.lld' + EXE)), '-flavor', 'wasm', '--no-entry', '--export=test', str(OUT / 'upload_caches.o'), '-o', str(OUT / 'upload_caches.wasm')], check=True)
runner = OUT / 'upload_caches.cjs'
runner.write_text("const fs = require('fs'); WebAssembly.instantiate(fs.readFileSync(process.argv[2]), {}).then(({instance}) => { const failure = instance.exports.test(); if (failure) throw new Error('Upload cache / descriptor check ' + failure + ' failed'); console.log('Upload caches and descriptor plans: checks passed (actual C helpers, mocked allocation/submissions)'); }).catch(e => { console.error(e); process.exitCode = 1; });")
subprocess.run(['node', str(runner), str(OUT / 'upload_caches.wasm')], check=True)
