// Keep buffer copies on the GPU timeline. CPU-side consumers wait only when
// their input intersects a not-yet-completed destination (as in the upstream
// deferred-copy designs); a later GPU consumer is ordered by the barriers.
#include "vk.h"
#include "timing_trace.h"
#include "../renderer.h"
#include <stdlib.h>
#ifdef __ANDROID__
typedef struct { uint64_t src, dst, size, seq; } PendingCopy;
static PendingCopy pending[256];
static unsigned npending;
static uint64_t issued;
static bool enabled(void) {
    static int on = -1;
    if (on < 0) { const char *value = getenv("LARECOMP_VK_ASYNC_BUFFER_COPY"); on = value && atoi(value) != 0; }
    return on != 0;
}
static void retire(uint64_t done) {
    unsigned keep = 0;
    for (unsigned i = 0; i < npending; ++i) if (pending[i].seq > done) pending[keep++] = pending[i];
    npending = keep;
}
bool vk_buffer_copy_pending(void) {
    if (npending) retire(vk_completed_seq());
    return npending != 0;
}
static void cpu_access(uint64_t addr, uint64_t size, bool write) {
    if (!npending || !size || addr > UINT64_MAX - size) return;
    uint64_t needed = 0;
    for (unsigned i = 0; i < npending; ++i)
        if (((pending[i].dst < addr + size && addr < pending[i].dst + pending[i].size) ||
             (write && pending[i].src < addr + size && addr < pending[i].src + pending[i].size)) && pending[i].seq > needed)
            needed = pending[i].seq;
    if (!needed) return;
    uint64_t done = vk_completed_seq();
    if (done < needed) {
        if (vk_timing_trace_on()) {
            static unsigned logged;
            if (logged++ < 64) LOG("[buffer-copy-dependency] %s %llx+%llx seq %llu done %llu\n",
                write ? "write" : "read", (unsigned long long)addr, (unsigned long long)size,
                (unsigned long long)needed, (unsigned long long)done);
        }
        vk_buffer_copy_wait();
        done = vk_completed_seq();
        if (done < needed) FATAL("[buffer-copy] CPU input is still in flight after waiting\n");
    }
    retire(done);
}
void vk_buffer_copy_cpu_read(uint64_t addr, uint64_t size) { cpu_access(addr, size, false); }
void vk_buffer_copy_cpu_write(uint64_t addr, uint64_t size) { cpu_access(addr, size, true); }
bool vk_buffer_copy_async(uint64_t src, uint64_t dst, uint64_t size) {
    if (!enabled() || !size || ((src | dst | size) & 3) || src > UINT64_MAX - size || dst > UINT64_MAX - size) return false;
    if (src == dst) return true;
    retire(vk_completed_seq());
    if (npending == sizeof pending / sizeof *pending) {
        vk_buffer_copy_wait(); retire(vk_completed_seq());
        if (npending == sizeof pending / sizeof *pending) return false;
    }
    // Any wait must precede the lookup: it records the backing's last-use seq.
    VkBuffer source, destination;
    VkDeviceSize so, offset;
    if (!vk_guest_shared_buffer(src, size, &source, &so) || !vk_guest_shared_buffer(dst, size, &destination, &offset)) return false;
    if (source == destination && so < offset + size && offset < so + size) return false; // Vulkan copy is not memmove
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferCopy copy = {so, offset, size};
    vkCmdCopyBuffer(cb, source, destination, 1, &copy);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cb, &dependency);
    uint64_t seq = vk_pending_seq();
    // Keep source reads too: an inline CPU update cannot overwrite an in-flight
    // source, even if a later copy replaced the same destination.
    pending[npending++] = (PendingCopy){src, dst, size, seq};
    if(!issued++)LOG("[buffer-copy] deferred GPU path active (first copy %llu bytes)\n",(unsigned long long)size);
    vk_memory_written(dst, size);
    vk_texture_memory_written(dst, size);
    return true;
}
void vk_buffer_copy_reset(void) { npending = 0; issued = 0; }
#endif
