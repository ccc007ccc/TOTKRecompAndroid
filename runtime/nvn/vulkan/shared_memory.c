// GPU-visible backing for Horizon's separately mapped resource memory.
// Like MacNativeGames/D2R's registered no-copy pools, address lookup retains the
// original guest address. Vulkan placement is done before the guest fills a new
// physical mapping, so there is no migration copy on the rendering path.
#include "vk.h"
#include "../renderer.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../platform.h"
#ifdef __ANDROID__
#include "../../settings.h"

typedef struct {
    uint64_t lo, size, live_pages, last_use;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *present; // Horizon 4 KiB pages; partial unmaps keep surviving CPU mappings intact
} SharedRegion;
static SharedRegion *regions;
static size_t count, capacity;
static pthread_mutex_t region_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t allocated;
// Partial releases retain their backing to preserve adjacent CPU mappings.
// Bound those retained allocations; later mappings fall back to CPU storage.
static const uint64_t backing_limit=512ull*1024*1024;

bool vk_shared_memory_active(void) {
    pthread_mutex_lock(&region_lock);
    bool active = count != 0;
    pthread_mutex_unlock(&region_lock);
    return active;
}

void vk_shared_memory_stats(void) {
    pthread_mutex_lock(&region_lock);
    uint64_t live = 0;
    for (size_t i = 0; i < count; ++i) live += regions[i].live_pages * 4096;
    LOG("[shared-memory-stats] %zu ranges, backing %.1f MiB, live %.1f MiB, retained holes %.1f MiB\n",
        count, allocated / 1048576.0, live / 1048576.0, (allocated - live) / 1048576.0);
    pthread_mutex_unlock(&region_lock);
}

bool vk_shared_memory_enabled(void) {
    const char *value = getenv("LARECOMP_VK_SHARED_ALIAS");
    // Keep opt-in until the mapping and partial-release path pass device checks.
    return value && atoi(value);
}

static bool create_region(uint64_t addr, uint64_t size) { // region_lock held
    if (size < 0x10000) return false; // small allocator pages do not each need a Vulkan allocation
    // Leave allocation slots for textures, transient rings and the shared heap.
    // Falling back for a new region leaves the previous mappings intact.
    uint32_t budget = g_vk.props.limits.maxMemoryAllocationCount / 2;
    if (budget > 4096) budget = 4096;
    if (count >= budget || size>backing_limit || allocated>backing_limit-size) return false;
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t alignment = g_vk.placed_align > page ? g_vk.placed_align : page;
    if (!alignment || (addr % alignment) || (size % alignment)) return false;
    size_t at = 0;
    while (at < count && regions[at].lo < addr) ++at;
    if ((at && regions[at - 1].lo + regions[at - 1].size > addr) || (at < count && addr + size > regions[at].lo)) {
        return false;
    }
    if (count == capacity) {
        size_t next = capacity ? capacity * 2 : 32;
        SharedRegion *p = realloc(regions, next * sizeof *p);
        if (!p) return false;
        regions = p; capacity = next;
    }
    SharedRegion region = {.lo = addr, .size = size, .live_pages = size / 4096};
    region.present = malloc(region.live_pages);
    if (!region.present) return false;
    memset(region.present, 1, region.live_pages);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkResult result = vkCreateBuffer(g_vk.dev, &bi, NULL, &region.buffer);
    if (result == VK_SUCCESS) {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_vk.dev, region.buffer, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &props);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits & (1u << i)) || (props.memoryTypes[i].propertyFlags & required) != required) continue;
            if (type == UINT32_MAX || (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) type = i;
            if (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) break;
        }
        // A whole placed mapping must not overwrite bytes beyond this new range.
        if (type == UINT32_MAX || req.size != size) result = VK_ERROR_FEATURE_NOT_PRESENT;
        else {
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = req.size, .memoryTypeIndex = type};
            result = vkAllocateMemory(g_vk.dev, &ai, NULL, &region.memory);
        }
    }
    if (result == VK_SUCCESS) result = vkBindBufferMemory(g_vk.dev, region.buffer, region.memory, 0);
    void *mapped = NULL;
    if (result == VK_SUCCESS) {
        VkMemoryMapPlacedInfoEXT placed = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
            .pPlacedAddress = (void *)(uintptr_t)addr};
        VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &placed,
            .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = region.memory, .size = VK_WHOLE_SIZE};
        result = vkMapMemory2KHR(g_vk.dev, &mi, &mapped);
    }
    if (result != VK_SUCCESS || mapped != (void *)(uintptr_t)addr) {
        if (region.buffer) vkDestroyBuffer(g_vk.dev, region.buffer, NULL);
        if (region.memory) vkFreeMemory(g_vk.dev, region.memory, NULL);
        if (mapped == (void *)(uintptr_t)addr && !plat_reset_guest(addr, addr + size))
            FATAL("[shared-memory] cannot restore failed placement\n");
        LOG("[shared-memory] placement unavailable at %llx+%llx (%d); using CPU memory\n",
            (unsigned long long)addr, (unsigned long long)size, result);
        free(region.present); return false;
    }
    memmove(regions + at + 1, regions + at, (count - at) * sizeof *regions);
    regions[at] = region; ++count; allocated += size;
    if (count == 1 || !(count % 64)) LOG("[shared-memory] mapped %llx+%llx; %.1f MiB in %zu ranges, alignment %llu\n",
        (unsigned long long)addr, (unsigned long long)size, allocated / 1048576.0, count, (unsigned long long)alignment);
    return true;
}

bool vk_shared_memory_place(uint64_t addr, uint64_t size) {
    if (!vk_shared_memory_enabled() || !g_vk.dev || !g_vk.map_placed || !size || ((addr | size) & 4095) || addr < ALIAS_BASE || size > ALIAS_SIZE ||
        addr - ALIAS_BASE > ALIAS_SIZE - size) return false;
    uint64_t end = addr + size;
    pthread_mutex_lock(&region_lock);
    // The kernel only supplies new, previously unmapped pages. Reject accidental
    // overlap without changing existing contents or the backing's lifetime.
    for (size_t i = 0; i < count; ++i) {
        SharedRegion *r = &regions[i];
        uint64_t lo = addr > r->lo ? addr : r->lo, hi = end < r->lo + r->size ? end : r->lo + r->size;
        for (uint64_t p = lo; p < hi; p += 4096) if (r->present[(p - r->lo) / 4096]) {
            pthread_mutex_unlock(&region_lock); return false;
        }
    }
    bool all = true;
    for (uint64_t cursor = addr; cursor < end;) {
        size_t i = 0;
        while (i < count && regions[i].lo + regions[i].size <= cursor) ++i;
        if (i < count && regions[i].lo <= cursor) {
            SharedRegion *r = &regions[i];
            uint64_t hi = end < r->lo + r->size ? end : r->lo + r->size;
            for (uint64_t p = cursor; p < hi; p += 4096) r->present[(p - r->lo) / 4096] = 1;
            r->live_pages += (hi - cursor) / 4096;
            cursor = hi;
        } else {
            uint64_t hi = i < count && regions[i].lo < end ? regions[i].lo : end;
            if (!create_region(cursor, hi - cursor)) all = false;
            cursor = hi;
        }
    }
    pthread_mutex_unlock(&region_lock);
    return all;
}

bool vk_shared_memory_lookup(uint64_t addr, uint64_t size, VkBuffer *buffer, VkDeviceSize *offset) {
    if (!size || addr < ALIAS_BASE || size > ALIAS_SIZE || addr - ALIAS_BASE > ALIAS_SIZE - size) return false;
    pthread_mutex_lock(&region_lock);
    size_t lo = 0, hi = count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (regions[mid].lo <= addr) lo = mid + 1; else hi = mid; }
    bool found = lo && addr - regions[lo - 1].lo <= regions[lo - 1].size &&
        size <= regions[lo - 1].size - (addr - regions[lo - 1].lo);
    if (found && regions[lo - 1].live_pages != regions[lo - 1].size / 4096) {
        const SharedRegion *r = &regions[lo - 1];
        for (uint64_t p = (addr - r->lo) / 4096; p <= (addr - r->lo + size - 1) / 4096; ++p)
            if (!r->present[p]) { found = false; break; }
    }
    if (found) {
        regions[lo - 1].last_use = vk_pending_seq();
        *buffer = regions[lo - 1].buffer; *offset = addr - regions[lo - 1].lo;
    }
    pthread_mutex_unlock(&region_lock);
    return found;
}

typedef struct { uint64_t addr, size; bool success; } Release;
static void release_range(void *arg) {
    Release *release = arg;
    uint64_t end = release->addr + release->size;
    pthread_mutex_lock(&region_lock);
    bool overlaps = false;
    uint64_t last_use = 0;
    for (size_t i = 0; i < count; ++i) if (regions[i].lo < end && release->addr < regions[i].lo + regions[i].size) {
        overlaps = true;
        if (regions[i].last_use > last_use) last_use = regions[i].last_use;
    }
    if (!overlaps) { release->success = true; pthread_mutex_unlock(&region_lock); return; }
    // Runs in order on the render thread. In-flight commands must finish before
    // removing their backing, including a partially unmapped resource allocation.
    if (last_use && vk_completed_seq() < last_use) {
        renderer_flush();
        if (vk_completed_seq() < last_use) { pthread_mutex_unlock(&region_lock); return; }
    }
    size_t keep = 0;
    for (size_t i = 0; i < count; ++i) {
        SharedRegion r = regions[i];
        uint64_t lo = release->addr > r.lo ? release->addr : r.lo;
        uint64_t hi = end < r.lo + r.size ? end : r.lo + r.size;
        for (uint64_t p = lo; p < hi; p += 4096) {
            uint64_t page = (p - r.lo) / 4096;
            if (r.present[page]) { r.present[page] = 0; --r.live_pages; }
        }
        if (!r.live_pages) {
            vkDestroyBuffer(g_vk.dev, r.buffer, NULL);
            vkFreeMemory(g_vk.dev, r.memory, NULL);
            if (!plat_reset_guest(r.lo, r.lo + r.size)) FATAL("[shared-memory] cannot restore released mapping\n");
            free(r.present);
            allocated -= r.size;
        } else regions[keep++] = r;
    }
    count = keep;
    release->success = true;
    pthread_mutex_unlock(&region_lock);
}

bool vk_shared_memory_reset_cpu(uint64_t lo, uint64_t hi) {
    // Do not replace individual pages of a live Vulkan mapping. The kernel's
    // subsequent MapPhysicalMemory zeroes those pages before returning them.
    pthread_mutex_lock(&region_lock);
    bool ok = true;
    uint64_t cursor = lo;
    for (size_t i = 0; i < count && cursor < hi; ++i) {
        const SharedRegion *r = &regions[i];
        if (r->lo + r->size <= cursor) continue;
        uint64_t stop = r->lo < hi ? r->lo : hi;
        if (cursor < stop && !plat_reset_guest(cursor, stop)) { ok = false; break; }
        if (r->lo >= hi) { cursor = hi; break; }
        cursor = r->lo + r->size;
    }
    if (ok && cursor < hi) ok = plat_reset_guest(cursor, hi);
    pthread_mutex_unlock(&region_lock);
    return ok;
}

bool vk_shared_memory_unmap(uint64_t addr, uint64_t size) {
    if (!size || ((addr | size) & 4095) || addr > UINT64_MAX - size) return false;
    pthread_mutex_lock(&region_lock);
    bool any = count != 0;
    pthread_mutex_unlock(&region_lock);
    if (!any) return true;
    Release release = {addr, size, false};
    vk_async_call_sync(release_range, &release);
    return release.success;
}

void vk_shared_memory_reset(void) { // caller already drained the GPU; the whole guest window is reset next
    pthread_mutex_lock(&region_lock);
    for (size_t i = 0; i < count; ++i) {
        vkDestroyBuffer(g_vk.dev, regions[i].buffer, NULL);
        vkFreeMemory(g_vk.dev, regions[i].memory, NULL);
        free(regions[i].present);
    }
    free(regions); regions = NULL; count = capacity = 0; allocated = 0;
    pthread_mutex_unlock(&region_lock);
}
#endif
