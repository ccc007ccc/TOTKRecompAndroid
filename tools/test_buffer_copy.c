// Delayed mock GPU: recorded copies become CPU visible only on completion.
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/nvn/vulkan/buffer_copy.c"

typedef struct { unsigned src, dst, size; } Copy;
static uint8_t memory[8192];
static Copy copies[512];
static unsigned ncopies, waits, invalidations, texture_writes, barriers, queries;
static uint64_t done, seq = 1;
static bool timeout;
static jmp_buf fatal;
static void complete(void) {
    for (unsigned i = 0; i < ncopies; ++i)
        memcpy(memory + copies[i].dst, memory + copies[i].src, copies[i].size);
    ncopies = 0; done = seq++;
}
uint64_t vk_completed_seq(void) { ++queries; return done; }
bool vk_timing_trace_on(void) { return false; }
uint64_t vk_pending_seq(void) { return seq; }
void vk_buffer_copy_wait(void) { ++waits; if (!timeout) complete(); }
void vk_end_pass(void) {}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)1; }
void vk_full_barrier(VkCommandBuffer cb) { assert(cb == (VkCommandBuffer)1); ++barriers; }
bool vk_guest_shared_buffer(uint64_t addr, uint64_t size, VkBuffer *buffer, VkDeviceSize *offset) {
    if (addr < 0x10000 || addr >= 0x12000 || size > 0x12000 - addr) return false;
    *buffer = (VkBuffer)1; *offset = addr - 0x10000; return true;
}
static void record(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    assert(cb && src == (VkBuffer)1 && dst == src && n == 1 && ncopies < 512);
    copies[ncopies++] = (Copy){r->srcOffset, r->dstOffset, r->size};
}
static void barrier(VkCommandBuffer cb, const VkDependencyInfo *info) {
    assert(cb && info->memoryBarrierCount == 1);
    const VkMemoryBarrier2 *b = info->pMemoryBarriers;
    assert(b->srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT);
    assert(b->dstStageMask & VK_PIPELINE_STAGE_2_HOST_BIT);
    assert(b->dstAccessMask & VK_ACCESS_2_HOST_READ_BIT);
    assert(b->dstAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT);
}
PFN_vkCmdCopyBuffer vkCmdCopyBuffer = record;
PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2 = barrier;
void vk_memory_written(uint64_t addr, uint64_t size) { assert(addr && size); ++invalidations; }
void vk_texture_memory_written(uint64_t addr, uint64_t size) { assert(addr && size); ++texture_writes; }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; longjmp(fatal, 1); }

int main(int argc, char **argv) {
    bool off = argc > 1 && !strcmp(argv[1], "off");
    setenv("LARECOMP_VK_ASYNC_BUFFER_COPY", off ? "0" : "1", 1);
    memset(memory, 0x31, 32);
    if (off) { assert(!vk_buffer_copy_async(0x10000, 0x10100, 32)); puts("PASS: GPU buffer copy disabled"); return 0; }
    assert(!vk_buffer_copy_async(0x10001, 0x10100, 32));
    assert(!vk_buffer_copy_async(0x10000, 0x10100, 3));
    assert(!vk_buffer_copy_async(UINT64_MAX - 3, 0x10100, 32));
    assert(!vk_buffer_copy_async(0x13000, 0x10100, 32));
    assert(!vk_buffer_copy_async(0x10000, 0x10004, 32));
    assert(vk_buffer_copy_async(0x10000, 0x10000, 32) && !npending);
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    assert(vk_buffer_copy_async(0x10100, 0x10200, 32));
    assert(!waits && !memory[0x200] && ncopies == 2); // still queued, second copy consumes first on GPU
    vk_buffer_copy_cpu_read(0x11000, 32);
    vk_buffer_copy_cpu_read(0x10000, 32); // reading the source is safe
    assert(!waits);
    vk_buffer_copy_cpu_read(0x1021c, 8); // partial overlap, CPU must see both ordered copies
    assert(waits == 1 && memory[0x200] == 0x31 && !vk_buffer_copy_pending());
    assert(invalidations == 2 && texture_writes == 2 && barriers == 2);
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    assert(vk_buffer_copy_async(0x10200, 0x10100, 32)); // same destination must retain first source lifetime
    vk_buffer_copy_cpu_write(0x10000, 4);
    assert(waits == 2 && !vk_buffer_copy_pending());
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    complete(); unsigned before = waits;
    vk_buffer_copy_cpu_read(0x10100, 32);
    assert(waits == before && !vk_buffer_copy_pending());
    for (unsigned i = 0; i < 257; ++i) assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    assert(waits == before + 1 && npending == 1); // bounded queue pressure completes old work
    timeout = true;
    if (!setjmp(fatal)) { vk_buffer_copy_cpu_read(0x10100, 32); assert(!"unfinished data was exposed"); }
    assert(npending == 1);
    timeout = false; vk_buffer_copy_cpu_write(0, UINT64_MAX);
    assert(!vk_buffer_copy_pending());
    // Uniform reads use a later submission than their producing copy. A CPU
    // write to even the uncopied prefix must wait for that read, not just copy.
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    vk_buffer_copy_uniform_begin();
    unsigned ticket = vk_buffer_copy_uniform_reserve(0x10100, 32);
    assert(ticket && vk_buffer_copy_uniform_reserve(0x10100, 32) == ticket);
    unsigned query_before = queries;
    for (unsigned i = 0; i < 100; ++i) assert(vk_buffer_copy_pending() && vk_buffer_copy_cpu_pending());
    assert(queries == query_before); // hot predicates never query the driver
    complete();
    vk_buffer_copy_retire(done); // existing submission poll; preserve reserved ticket
    assert(queries == query_before);
    assert(!vk_buffer_copy_pending() && vk_buffer_copy_cpu_pending());
    query_before = queries;
    vk_buffer_copy_uniform_use(ticket);
    assert(queries == query_before);
    VkBufferCopy uniform_read = {0x100, 0x300, 32};
    record((VkCommandBuffer)1, (VkBuffer)1, (VkBuffer)1, 1, &uniform_read);
    before = waits;
    vk_buffer_copy_cpu_read(0x10100, 32); // read/read is safe
    vk_buffer_copy_cpu_write(0x10120, 4); // adjacency is not overlap
    assert(waits == before);
    vk_buffer_copy_uniform_begin(); // previous draw no longer reserves tickets
    vk_buffer_copy_cpu_write(0x10100, 4);
    assert(waits == before + 1 && memory[0x310] == 0x31 && !vk_buffer_copy_cpu_pending());
    memset(memory + 0x100, 0x99, 32);
    assert(memory[0x310] == 0x31); // the GPU observed the old uniform
    assert(!vk_buffer_copy_uniform_reserve(UINT64_MAX - 4, 32));
    assert(!vk_buffer_copy_uniform_reserve(0x10100, 0));
    // Full table: fail closed, without waiting inside descriptor binding or
    // evicting reservations. Retired, unreserved entries can then be reused.
    assert(vk_buffer_copy_async(0x10000, 0x10800, 1024));
    vk_buffer_copy_uniform_begin();
    for (unsigned i = 0; i < 64; ++i) {
        ticket = vk_buffer_copy_uniform_reserve(0x10800 + i * 4, 16);
        assert(ticket); vk_buffer_copy_uniform_use(ticket);
    }
    before = waits;
    assert(!vk_buffer_copy_uniform_reserve(0x10980, 16) && waits == before);
    complete();
    assert(!vk_buffer_copy_uniform_reserve(0x10980, 16)); // reservations survive retirement
    vk_buffer_copy_uniform_begin();
    assert(vk_buffer_copy_async(0x10000, 0x10800, 1024));
    assert(vk_buffer_copy_uniform_reserve(0x10980, 16));
    vk_buffer_copy_cpu_write(0, UINT64_MAX);
    vk_buffer_copy_reset(); assert(!npending);
    assert(!vk_buffer_copy_cpu_pending());
    puts("PASS: chained copies, CPU visibility, source/uniform read lifetime, stable reservations, bounds/capacity fallback, retirement and wait failure");
}
