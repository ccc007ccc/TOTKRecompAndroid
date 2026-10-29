"""Production global arena allocation under cached non-coherent/coherent VMA choices."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / '.work/totk-port/arena-memory-tests').resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('static bool gres_init(')
code = source[start:source.index('\n}', start) + 2]
unit = out / 'global-arena-memory.c'
unit.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>
#endif
#define LOG(...) ((void)0)
#define GA_SIZE (128ull << 20)
static struct { VmaAllocator vma; VkPhysicalDeviceProperties props; } g_vk;
static int gres_state = -1;
static VkBuffer ga_buf;
static VmaAllocation ga_mem;
static uint8_t *ga_ptr, mapped;
static VkMemoryPropertyFlags offered[2];
static unsigned calls, destroyed, selected;
static bool map_missing;
VKAPI_ATTR VkResult VKAPI_CALL vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo *b,
        const VmaAllocationCreateInfo *c, VkBuffer *buffer, VmaAllocation *memory, VmaAllocationInfo *info) {
    (void)a; assert(b->size == GA_SIZE); ++calls;
    assert(c->flags & VMA_ALLOCATION_CREATE_MAPPED_BIT);
    for (unsigned i = 0; i < 2; ++i) if ((offered[i] & c->requiredFlags) == c->requiredFlags) {
        *buffer = (VkBuffer)1; *memory = (VmaAllocation)2;
        *info = (VmaAllocationInfo){.memoryType = i, .pMappedData = map_missing ? NULL : &mapped};
        selected = i; return VK_SUCCESS;
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR void VKAPI_CALL vmaGetMemoryTypeProperties(VmaAllocator a, uint32_t index, VkMemoryPropertyFlags *flags) {
    (void)a; *flags = offered[index];
}
VKAPI_ATTR void VKAPI_CALL vmaDestroyBuffer(VmaAllocator a, VkBuffer b, VmaAllocation m) {
    (void)a; assert(b == (VkBuffer)1 && m == (VmaAllocation)2); ++destroyed;
}
''' + code + r'''
int main(void) {
    g_vk.props.limits.maxStorageBufferRange = GA_SIZE;
    offered[0] = 0xb; offered[1] = 0x7; // cached non-coherent, then uncached coherent (Mali)
    assert(gres_init() && selected == 1 && ga_ptr == &mapped);
    assert(gres_init() && calls == 1);
    gres_state = -1; offered[0] = 0xf; // cached coherent stays preferred (Turnip)
    assert(gres_init() && selected == 0);
    gres_state = -1; offered[0] = offered[1] = 0xb;
    assert(!gres_init() && !ga_buf && !ga_mem);
    unsigned before = calls;
    assert(!gres_init() && calls == before); // safe fallback is cached
    gres_state = -1; offered[0] = 0x7; map_missing = true;
    assert(!gres_init() && !ga_buf && !ga_mem && destroyed == 1);
    gres_state = -1; g_vk.props.limits.maxStorageBufferRange = GA_SIZE - 1;
    before = calls; assert(!gres_init() && calls == before);
    puts("PASS: arena rejects non-coherent memory, keeps cached coherent memory, and falls back/cleans up on failure");
}
''')
binary = out / 'test-global-arena-memory'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined',
                '-I' + str(root / 'android/deps/out/include'), str(unit), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=10)
