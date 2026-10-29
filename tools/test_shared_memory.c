// Production mapping lifetime with real mmap and simulated Vulkan allocations.
// GPU numeric execution is tested separately on the phone.
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include "../runtime/nvn/vulkan/shared_memory.c"

VkG g_vk;
uint64_t g_guest_lo;
typedef struct { size_t size; void *address; } Memory;
typedef struct { size_t size; Memory *memory; } Buffer;
static unsigned creates, frees, waits;
static int fail_alloc, bad_requirements, timeout;
static VkResult create_buffer(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBuffer *out) {
    (void)dev; (void)alloc; Buffer *b = calloc(1, sizeof *b); assert(b);
    b->size = ci->size; *out = (VkBuffer)b; creates++; return VK_SUCCESS;
}
static void destroy_buffer(VkDevice dev, VkBuffer buf, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc; free((void *)buf);
}
static void requirements(VkDevice dev, VkBuffer buf, VkMemoryRequirements *out) {
    (void)dev; *out = (VkMemoryRequirements){.size = ((Buffer *)buf)->size + (bad_requirements ? 4096 : 0), .alignment = 4096, .memoryTypeBits = 3};
}
static void memory_properties(VkPhysicalDevice dev, VkPhysicalDeviceMemoryProperties *out) {
    (void)dev; memset(out, 0, sizeof *out); out->memoryTypeCount = 2;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    out->memoryTypes[1].propertyFlags = out->memoryTypes[0].propertyFlags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}
static VkResult allocate_memory(VkDevice dev, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
    (void)dev; (void)alloc;
    if (fail_alloc) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(info->memoryTypeIndex == 1); Memory *m = calloc(1, sizeof *m); assert(m);
    m->size = info->allocationSize; *out = (VkDeviceMemory)m; return VK_SUCCESS;
}
static void free_memory(VkDevice dev, VkDeviceMemory memory, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc; Memory *m = (Memory *)memory;
    if (m->address) assert(!munmap(m->address, m->size)); free(m); frees++;
}
static VkResult bind_memory(VkDevice dev, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)dev; assert(!offset); ((Buffer *)buffer)->memory = (Memory *)memory; return VK_SUCCESS;
}
static VkResult map_memory(VkDevice dev, const VkMemoryMapInfoKHR *info, void **out) {
    (void)dev; const VkMemoryMapPlacedInfoEXT *placed = info->pNext;
    assert(placed && info->flags == VK_MEMORY_MAP_PLACED_BIT_EXT);
    Memory *m = (Memory *)info->memory;
    m->address = mmap(placed->pPlacedAddress, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(m->address == placed->pPlacedAddress); *out = m->address; return VK_SUCCESS;
}
PFN_vkCreateBuffer vkCreateBuffer = create_buffer;
PFN_vkDestroyBuffer vkDestroyBuffer = destroy_buffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = requirements;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = memory_properties;
PFN_vkAllocateMemory vkAllocateMemory = allocate_memory;
PFN_vkFreeMemory vkFreeMemory = free_memory;
PFN_vkBindBufferMemory vkBindBufferMemory = bind_memory;
PFN_vkMapMemory2KHR vkMapMemory2KHR = map_memory;
void rt_fatal_message(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); abort();
}
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)(uintptr_t)lo;
}
static bool render_context;
static unsigned ordered_releases;
void vk_async_call_sync(void (*call)(void *), void *arg) {
    bool before=render_context;render_context=true;++ordered_releases;call(arg);render_context=before;
}
#define R_OUT_OF_RESOURCE 99
static uint32_t physical_memory_apply(uint64_t addr,uint64_t size,bool map) {
    (void)addr;(void)size;
    // A mapper may create the first region after the dispatcher has checked its
    // option. Unmap must already be in render order before taking the kernel lock.
    if(!map && vk_shared_memory_enabled())assert(render_context);
    return 7;
}
#include "physical_dispatch.inc"
static uint64_t sequence = 1;
void renderer_flush(void) { waits++; sequence++; }
uint64_t vk_completed_seq(void) { return timeout ? 0 : sequence; }
uint64_t vk_pending_seq(void) { return sequence + 1; }
static atomic_bool reading;
static void *read_survivor(void *arg) {
    volatile uint8_t *bytes = arg;
    while (atomic_load(&reading)) { assert(bytes[13] == 0x61); assert(bytes[0x9000] == 0x35); }
    return NULL;
}

int main(void) {
    void *reservation = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(reservation != MAP_FAILED);
    g_guest_lo = (uintptr_t)reservation - 0x100000000ull; // keep mappings in ASan's supported host address range
    uint64_t base = ALIAS_BASE;
    assert(plat_reset_guest(base, base + 0x100000));
    setenv("LARECOMP_VK_SHARED_ALIAS", "1", 1);
    assert(!vk_shared_memory_active());
    assert(physical_memory(base,4096,false)==7 && ordered_releases==1);
    assert(physical_memory(base,4096,true)==7 && ordered_releases==1);
    setenv("LARECOMP_VK_SHARED_ALIAS","0",1);
    assert(physical_memory(base,4096,false)==7 && ordered_releases==1);
    setenv("LARECOMP_VK_SHARED_ALIAS","1",1);
    g_vk.dev = (VkDevice)1; g_vk.map_placed = true; g_vk.placed_align = 4096;
    g_vk.props.limits.maxMemoryAllocationCount = 4096;
    assert(!vk_shared_memory_place(base + 1, 4096));
    assert(!vk_shared_memory_place(UINT64_MAX - 3, 4096));
    assert(vk_shared_memory_place(base + 0x10000, 0x10000));
    assert(vk_shared_memory_place(base, 0x10000)); // insertion before existing entry
    assert(vk_shared_memory_active());
    g_vk.props.limits.maxMemoryAllocationCount = 4;
    assert(!vk_shared_memory_place(base + 0x30000, 0x10000)); // reserve driver allocation slots
    g_vk.props.limits.maxMemoryAllocationCount = 4096;
    uint64_t real_allocated=allocated;allocated=backing_limit;
    assert(!vk_shared_memory_place(base+0x30000,0x10000));
    allocated=real_allocated;
    VkBuffer b, other; VkDeviceSize offset;
    assert(vk_shared_memory_lookup(base + 13, 100, &b, &offset) && offset == 13);
    memset((void *)(uintptr_t)base, 0x35, 0x10000);
    ((uint8_t *)((Buffer *)b)->memory->address)[13] = 0x61;
    assert(*(uint8_t *)(uintptr_t)(base + 13) == 0x61);
    assert(!vk_shared_memory_place(base + 4096, 4096));
    assert(!vk_shared_memory_lookup(base + 0xffff, 2, &b, &offset));
    assert(!vk_shared_memory_lookup(base, 0, &b, &offset));
    assert(!vk_shared_memory_lookup(UINT64_MAX - 1, 5, &b, &offset));
    assert(vk_shared_memory_lookup(base + 0x10000, 4096, &other, &offset));
    memset((void *)(uintptr_t)(base + 0x10000), 0x79, 0x10000);
    timeout = 1; unsigned before = frees;
    assert(!vk_shared_memory_unmap(base + 0x4000, 0x4000));
    assert(frees == before && vk_shared_memory_lookup(base, 16, &b, &offset));
    timeout = 0;
    atomic_store(&reading, true);
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, read_survivor, (void *)(uintptr_t)base));
    assert(vk_shared_memory_unmap(base + 0x4000, 0x4000));
    assert(vk_shared_memory_reset_cpu(base + 0x4000, base + 0x8000));
    assert(vk_shared_memory_lookup(base, 16, &b, &offset));
    assert(!vk_shared_memory_lookup(base + 0x3000, 0x2000, &b, &offset));
    assert(*(uint8_t *)(uintptr_t)(base + 13) == 0x61);
    assert(*(uint8_t *)(uintptr_t)(base + 0x3000) == 0x35);
    assert(*(uint8_t *)(uintptr_t)(base + 0x9000) == 0x35);
    assert(vk_shared_memory_lookup(base + 0x10000, 4096, &b, &offset) && b == other);
    assert(*(uint8_t *)(uintptr_t)(base + 0x10000) == 0x79);
    for (unsigned i = 0; i < 30; ++i) {
        assert(vk_shared_memory_place(base + 0x4000, 0x4000));
        memset((void *)(uintptr_t)(base + 0x4000), 0, 0x4000); // kernel initializes newly mapped pages
        assert(vk_shared_memory_lookup(base, 0x10000, &b, &offset));
        assert(vk_shared_memory_unmap(base + 0x4000, 0x4000));
        assert(vk_shared_memory_reset_cpu(base + 0x4000, base + 0x8000));
    }
    atomic_store(&reading, false); assert(!pthread_join(reader, NULL));
    fail_alloc = 1;
    assert(!vk_shared_memory_place(base + 0x30000, 0x10000));
    assert(*(uint8_t *)(uintptr_t)(base + 0x3000) == 0x35);
    fail_alloc = 0; bad_requirements = 1;
    assert(!vk_shared_memory_place(base + 0x30000, 0x10000));
    bad_requirements = 0;
    assert(vk_shared_memory_unmap(base + 0x10000, 0x10000));
    assert(vk_shared_memory_unmap(base, 0x10000));
    assert(count == 0 && allocated == 0);
    assert(!vk_shared_memory_active());
    assert(vk_shared_memory_place(base, 0x10000));
    vk_shared_memory_reset();
    assert(!vk_shared_memory_lookup(base, 16, &b, &offset) && !count && !allocated);
    assert(plat_reset_guest(base, base + 0x100000));
    assert(!munmap((void *)(uintptr_t)base, 0x100000));
    puts("PASS: shared-address lookup, overlap/bounds, allocation fallback, GPU wait failure, partial unmap preservation, remap and reset");
}
