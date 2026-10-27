// Included by test_writeback.py with the write-back block extracted from exec.c unchanged.
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/vulkan/vk.h"

VkG g_vk;
static uint64_t submit_seq, completed;
static const char *wait_why;
static unsigned submits, waits, copies, invalidates;
static int timeout;
static jmp_buf fatal;
static unsigned char *mapped;
static size_t mapped_size;
static struct { uint64_t seq, off, size; unsigned char value; } pending[2048];
static unsigned pending_count, pending_done;
static void wb_apply(uint64_t done);
void renderer_submit(void);
uint64_t renderer_present_count(void) { return 1; }
uint64_t nvn_linear_rt_pitch(uint64_t bytes) { return (bytes + 127) & ~127ull; }
void rt_fatal_trace(void) { assert(timeout); longjmp(fatal, 1); }
void vk_device_fault_report(void) { abort(); }
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
void vk_full_barrier(VkCommandBuffer cb) { (void)cb; }
uint64_t vk_pending_seq(void) { return submit_seq + 1; }
uint64_t vk_completed_seq(void) { return completed; }
void renderer_submit(void) { submit_seq++; submits++; }
static void complete_to(uint64_t seq) {
    assert(seq <= submit_seq);
    while (pending_done < pending_count && pending[pending_done].seq <= seq) {
        unsigned i = pending_done++;
        memset(mapped + pending[i].off, pending[i].value, pending[i].size);
    }
    completed = seq;
}
void vk_wait_seq(uint64_t seq) {
    assert(seq <= submit_seq);
    assert(wait_why && !strcmp(wait_why, "write-back slots full"));
    waits++;
    if (!timeout) { complete_to(seq); wb_apply(seq); }
}
VKAPI_ATTR VkResult VKAPI_CALL vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo *bi,
        const VmaAllocationCreateInfo *ai, VkBuffer *buf, VmaAllocation *mem, VmaAllocationInfo *info) {
    (void)allocator; (void)ai;
    assert(!mapped);
    mapped_size = bi->size;
    mapped = calloc(1, mapped_size);
    assert(mapped);
    *buf = (VkBuffer)(uintptr_t)1; *mem = (VmaAllocation)(uintptr_t)1;
    info->pMappedData = mapped;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation,
        VkDeviceSize off, VkDeviceSize size) {
    (void)allocator; (void)allocation;
    assert(off + size <= mapped_size);
    invalidates++;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
        VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    (void)cb; (void)layout; (void)buffer;
    assert(count == 1 && pending_count < 2048);
    const VkBufferImageCopy *r = regions;
    uint64_t size = (uint64_t)r->imageExtent.width * r->imageExtent.height * 2; // test images: R16
    assert(r->bufferOffset + size <= mapped_size && !(r->bufferOffset & 3));
    pending[pending_count++] = (typeof(*pending)){submit_seq + 1, r->bufferOffset, size, (uintptr_t)image};
    copies++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *info) {
    (void)cb; (void)info;
}

#include "writeback-under-test.inc"

static NvnTexture texture(unsigned w, unsigned h, void *storage, uint64_t stride, TexBackend *backend) {
    NvnTexture t = {.width = w, .height = h, .depth = 1, .levels = 1, .format = 0x5,
        .flags = 0x40, .stride = stride, .addr = (uintptr_t)storage, .backend = backend,
        .layout = {.width = w, .height = h, .depth = 1, .levels = 1, .layers = 1, .fmt = nvn_format_info(0x5)}};
    tegra_layout_init(&t.layout);
    backend->width = w; backend->height = h; backend->samples = 1; backend->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    assert(wb_eligible(&t, backend, 0));
    return t;
}
static void enqueue(NvnTexture *t, unsigned value) {
    ((TexBackend *)t->backend)->image = (VkImage)(uintptr_t)value;
    wb_queue(t);
}
static void drain(void) { renderer_submit(); complete_to(submit_seq); wb_apply(completed); assert(!wb_count); }

int main(void) {
    unsigned char *guest = malloc(600000);
    assert(guest);
    memset(guest, 0xa5, 600000);
    TexBackend bs = {0}, bb = {0};
    NvnTexture small = texture(2, 2, guest, 16, &bs);
    NvnTexture big = texture(256, 256, guest, 512, &bb);

    // The small pool fits 128 copies without forcing the 32 large slots to wait.
    for (unsigned i = 0; i < 128; i++) enqueue(&small, 11);
    for (unsigned i = 0; i < 32; i++) enqueue(&big, 22);
    assert(!waits && !submits && copies == 160 && wb_count == 160);
    assert(guest[0] == 0xa5 && !invalidates);
    wb_apply(0); assert(guest[0] == 0xa5 && wb_count == 160);
    enqueue(&small, 33); // full pool submits the pending batch before waiting
    assert(waits == 1 && submits == 1 && copies == 161 && wb_count == 1);
    assert(guest[0] == 22 && stat_wb_dropped == 0);
    drain(); assert(guest[0] == 33 && guest[4] == 22);

    // Same submission, opposite pool order: latest copy must win in overlapping guest memory.
    enqueue(&big, 44); enqueue(&small, 55); drain();
    assert(guest[0] == 55 && guest[16] == 55 && guest[4] == 44);
    enqueue(&small, 66); enqueue(&big, 77); drain(); assert(guest[0] == 77);

    // Move the small ring cursor near wraparound, then enqueue repeated writes spanning it.
    for (unsigned i = 0; i < 120; i++) enqueue(&small, 1);
    drain();
    for (unsigned i = 1; i <= 16; i++) enqueue(&small, i);
    drain(); assert(guest[0] == 16); // old index-order retirement would leave an older value

    // Explicit RT pitch is retained and row padding untouched, including non-power-of-two dimensions.
    NvnTexture pitched = texture(68, 68, guest, 160, &bb);
    memset(guest, 0xa5, 160 * 68);
    enqueue(&pitched, 91); drain();
    for (unsigned y = 0; y < 68; y++) for (unsigned x = 0; x < 160; x++)
        assert(guest[y * 160 + x] == (x < 136 ? 91 : 0xa5));

    // Submitted work that is still running also waits; an uncompleted timeline is never treated as ready.
    for (unsigned i = 0; i < 128; i++) enqueue(&small, 101);
    renderer_submit();
    unsigned before_submit = submits, before_wait = waits;
    enqueue(&small, 102);
    assert(submits == before_submit && waits == before_wait + 1);
    drain(); assert(guest[0] == 102);

    for (unsigned i = 0; i < 128; i++) enqueue(&small, 103);
    timeout = 1;
    unsigned before_copy = copies, before_invalidate = invalidates;
    if (!setjmp(fatal)) { enqueue(&small, 104); assert(!"timeout must fail without reading GPU data"); }
    assert(copies == before_copy && invalidates == before_invalidate && wb_count == 128 && guest[0] == 102);
    timeout = 0; complete_to(submit_seq); wb_apply(completed);
    assert(!wb_count && guest[0] == 103 && !stat_wb_dropped);
    free(mapped); free(guest);
    puts("write-back queue passed: saturation, delayed completion, wraparound, cross-pool alias order, RT pitch, timeout");
}
