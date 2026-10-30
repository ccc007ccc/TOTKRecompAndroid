// Link the actual packet recorder and Volk; driver doubles check values after the
// producer has overwritten/freed its arguments. No Vulkan device or game data needed.
#include "../runtime/nvn/vulkan/command_worker.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE(T, n) ((T)(uintptr_t)(n))
static pthread_t producer;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool entered, released;
static _Atomic bool stress, backpressure;
static unsigned phase, event, draw_index, pipelines, fallback_index;
static bool failing_alloc, failing_thread;
static unsigned alloc_index, fail_index, live_allocs;
static void *allocs[16];

void *__real_malloc(size_t n);
void __real_free(void *p);
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
void *__wrap_malloc(size_t n) {
    if (failing_alloc && alloc_index++ == fail_index) return NULL;
    void *p = __real_malloc(n);
    if (failing_alloc && p) { assert(live_allocs < 16); allocs[live_allocs++] = p; }
    return p;
}
void __wrap_free(void *p) {
    for (unsigned i = 0; i < live_allocs; i++) if (allocs[i] == p) {
        allocs[i] = allocs[--live_allocs]; break;
    }
    __real_free(p);
}
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    return failing_thread ? EAGAIN : __real_pthread_create(t, a, fn, arg);
}
int __wrap_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (atomic_load(&stress) && pthread_equal(pthread_self(), producer)) {
        atomic_store(&backpressure, true);
        // The producer really reached a queue-full wait. Release the held consumer
        // only now, so this cannot pass by merely enqueueing fewer than 16 slots.
        pthread_mutex_lock(&gate); released = true; pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate);
    }
    return __real_pthread_cond_wait(c, m);
}
static void hold(void) {
    pthread_mutex_lock(&gate); entered = true; pthread_cond_broadcast(&gate_cv);
    while (!released) pthread_cond_wait(&gate_cv, &gate);
    pthread_mutex_unlock(&gate);
}
static void wait_entered(void) {
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&gate_cv, &gate);
    pthread_mutex_unlock(&gate);
}
static void release(void) {
    pthread_mutex_lock(&gate); released = true; pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate);
}
static void next(unsigned n) { assert(phase == 1 && ++event == n); assert(!pthread_equal(pthread_self(), producer)); }
static VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t first, uint32_t instance) {
    assert(cb == HANDLE(VkCommandBuffer, 1) && v == 3 && i == 1 && instance == 0);
    if (phase == 2) assert(first == draw_index++);
}
static VKAPI_ATTR void VKAPI_CALL pipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    assert(cb == HANDLE(VkCommandBuffer, 1) && bp == VK_PIPELINE_BIND_POINT_GRAPHICS && p == HANDLE(VkPipeline, 2));
    pipelines++;
}
static VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                      uint32_t off, uint32_t n, const void *data) {
    assert(cb == HANDLE(VkCommandBuffer, 1) && layout == HANDLE(VkPipelineLayout, 2) && stages == VK_SHADER_STAGE_VERTEX_BIT);
    const uint8_t *bytes = data;
    if (off == 0) { assert(n == 4096 && bytes[0] == 0x42 && bytes[n-1] == 0x42); hold(); }
    else if (off == 16) { next(1); assert(n == 16 && bytes[0] == 0x71 && bytes[15] == 0x71); }
    else {
        assert(off == 32 && n == 70000 && bytes[n-1] == 0x33 && pthread_equal(pthread_self(), producer));
        assert(pipelines == ++fallback_index);
    }
}
static VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                      uint32_t first, uint32_t ns, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn) {
    next(2); assert(cb == HANDLE(VkCommandBuffer, 1) && bp == VK_PIPELINE_BIND_POINT_GRAPHICS && layout == HANDLE(VkPipelineLayout, 2));
    assert(first == 0 && ns == 2 && sets[0] == HANDLE(VkDescriptorSet, 11) && sets[1] == HANDLE(VkDescriptorSet, 12));
    assert(nd == 3 && dyn[0] == 256 && dyn[1] == 512 && dyn[2] == 768);
}
static VKAPI_ATTR void VKAPI_CALL vertices(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs) {
    next(3); assert(cb == HANDLE(VkCommandBuffer, 1) && first == 4 && n == 2);
    assert(bufs[0] == HANDLE(VkBuffer, 23) && bufs[1] == HANDLE(VkBuffer, 24) && offs[0] == 64 && offs[1] == 0x123456789ull);
}
static VKAPI_ATTR void VKAPI_CALL rendering(VkCommandBuffer cb, const VkRenderingInfo *p) {
    next(4); assert(cb == HANDLE(VkCommandBuffer, 1) && !p->pNext && p->colorAttachmentCount == 2 && p->layerCount == 1);
    assert(p->pColorAttachments[0].imageView == HANDLE(VkImageView, 31) && p->pColorAttachments[1].clearValue.color.uint32[2] == 0x1234);
    assert(p->pDepthAttachment->imageView == HANDLE(VkImageView, 33) && p->pDepthAttachment->clearValue.depthStencil.depth == 0.25f);
    assert(p->pStencilAttachment->imageView == HANDLE(VkImageView, 34) && p->pStencilAttachment->clearValue.depthStencil.stencil == 0x77);
}
static VKAPI_ATTR void VKAPI_CALL barrier2(VkCommandBuffer cb, const VkDependencyInfo *p) {
    assert(cb == HANDLE(VkCommandBuffer, 1));
    if (p->pNext || (p->memoryBarrierCount && p->pMemoryBarriers[0].pNext)) {
        assert(pthread_equal(pthread_self(), producer) && pipelines == ++fallback_index); return;
    }
    next(5); assert(p->memoryBarrierCount == 1 && p->bufferMemoryBarrierCount == 1 && p->imageMemoryBarrierCount == 1);
    assert(p->pMemoryBarriers[0].srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT);
    assert(p->pBufferMemoryBarriers[0].buffer == HANDLE(VkBuffer, 41) && p->pBufferMemoryBarriers[0].offset == 0x100000001ull);
    assert(p->pImageMemoryBarriers[0].image == HANDLE(VkImage, 42) && p->pImageMemoryBarriers[0].subresourceRange.layerCount == 7);
}
static VKAPI_ATTR void VKAPI_CALL barrier1(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
    uint32_t ni, const VkImageMemoryBarrier *i) {
    next(6); assert(cb == HANDLE(VkCommandBuffer, 1) && src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT && flags == 0);
    assert(nm == 1 && nb == 1 && ni == 1 && m->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    assert(b->buffer == HANDLE(VkBuffer, 43) && i->image == HANDLE(VkImage, 44));
}
static VKAPI_ATTR void VKAPI_CALL renderpass(VkCommandBuffer cb, const VkRenderPassBeginInfo *p, VkSubpassContents contents) {
    next(7); assert(cb == HANDLE(VkCommandBuffer, 1) && contents == VK_SUBPASS_CONTENTS_INLINE);
    assert(p->framebuffer == HANDLE(VkFramebuffer, 51) && p->clearValueCount == 2 && p->pClearValues[1].color.uint32[3] == 0xaabbccdd);
}
static VKAPI_ATTR void VKAPI_CALL blend(VkCommandBuffer cb, const float *v) {
    next(8); assert(cb == HANDLE(VkCommandBuffer, 1) && v[0] == .125f && v[3] == .75f);
}
static VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer cb) {
    assert(cb == HANDLE(VkCommandBuffer, 1) && pthread_equal(pthread_self(), producer));
    return VK_ERROR_DEVICE_LOST;
}
static VKAPI_ATTR VkResult VKAPI_CALL reset(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags f) {
    assert(dev == HANDLE(VkDevice, 1) && pool == HANDLE(VkCommandPool, 2) && !f && draw_index == 40000);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void gate_command(void) {
    entered = released = false;
    uint8_t bytes[4096]; memset(bytes, 0x42, sizeof bytes);
    vkCmdPushConstants(HANDLE(VkCommandBuffer, 1), HANDLE(VkPipelineLayout, 2), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof bytes, bytes);
    wait_entered(); memset(bytes, 0, sizeof bytes);
}
static void snapshots(void) {
    VkCommandBuffer cb = HANDLE(VkCommandBuffer, 1);
    phase = 1; gate_command();
    uint8_t *bytes = malloc(16); memset(bytes, 0x71, 16);
    vkCmdPushConstants(cb, HANDLE(VkPipelineLayout, 2), VK_SHADER_STAGE_VERTEX_BIT, 16, 16, bytes); free(bytes);
    VkDescriptorSet sets[] = {HANDLE(VkDescriptorSet, 11), HANDLE(VkDescriptorSet, 12)};
    uint32_t dyn[] = {256, 512, 768};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, HANDLE(VkPipelineLayout, 2), 0, 2, sets, 3, dyn);
    memset(sets, 0, sizeof sets); memset(dyn, 0, sizeof dyn);
    VkBuffer bufs[] = {HANDLE(VkBuffer, 23), HANDLE(VkBuffer, 24)}; VkDeviceSize offs[] = {64, 0x123456789ull};
    vkCmdBindVertexBuffers(cb, 4, 2, bufs, offs); memset(bufs, 0, sizeof bufs); memset(offs, 0, sizeof offs);
    VkRenderingAttachmentInfo colors[2] = {{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = HANDLE(VkImageView, 31)},
        {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .clearValue.color.uint32 = {0, 0, 0x1234, 0}}};
    VkRenderingAttachmentInfo depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = HANDLE(VkImageView, 33), .clearValue.depthStencil.depth = .25f};
    VkRenderingAttachmentInfo stencil = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = HANDLE(VkImageView, 34), .clearValue.depthStencil.stencil = 0x77};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .layerCount = 1, .colorAttachmentCount = 2,
        .pColorAttachments = colors, .pDepthAttachment = &depth, .pStencilAttachment = &stencil};
    vkCmdBeginRendering(cb, &ri); memset(&ri, 0, sizeof ri); memset(colors, 0, sizeof colors); memset(&depth, 0, sizeof depth); memset(&stencil, 0, sizeof stencil);
    VkMemoryBarrier2 m2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT};
    VkBufferMemoryBarrier2 b2 = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, .buffer = HANDLE(VkBuffer, 41), .offset = 0x100000001ull};
    VkImageMemoryBarrier2 i2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .image = HANDLE(VkImage, 42), .subresourceRange.layerCount = 7};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m2,
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b2, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &i2};
    vkCmdPipelineBarrier2(cb, &di); memset(&di, 0, sizeof di); memset(&m2, 0, sizeof m2); memset(&b2, 0, sizeof b2); memset(&i2, 0, sizeof i2);
    VkMemoryBarrier m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    VkBufferMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .buffer = HANDLE(VkBuffer, 43)};
    VkImageMemoryBarrier i = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = HANDLE(VkImage, 44)};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &m, 1, &b, 1, &i);
    memset(&m, 0, sizeof m); memset(&b, 0, sizeof b); memset(&i, 0, sizeof i);
    VkClearValue clears[2] = {{0}, {.color.uint32 = {0, 0, 0, 0xaabbccdd}}};
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .framebuffer = HANDLE(VkFramebuffer, 51), .clearValueCount = 2, .pClearValues = clears};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE); memset(&rp, 0, sizeof rp); memset(clears, 0, sizeof clears);
    float constants[] = {.125f, .25f, .5f, .75f}; vkCmdSetBlendConstants(cb, constants); memset(constants, 0, sizeof constants);
    release(); assert(vkEndCommandBuffer(cb) == VK_ERROR_DEVICE_LOST); assert(event == 8);
}
int main(void) {
    producer = pthread_self();
    vkCmdDraw = draw; vkCmdBindPipeline = pipeline; vkCmdPushConstants = push; vkCmdBindDescriptorSets = bind;
    vkCmdBindVertexBuffers = vertices; vkCmdBeginRendering = rendering; vkCmdPipelineBarrier2 = barrier2;
    vkCmdPipelineBarrier = barrier1; vkCmdBeginRenderPass = renderpass; vkCmdSetBlendConstants = blend; vkEndCommandBuffer = end;
    vkResetCommandPool = reset;
    // Macro use must preserve PFN null checks, assignment and core/extension aliases.
    PFN_vkCmdDraw saved = vkCmdDraw; vkCmdDraw = NULL; assert(!vkCmdDraw && !vk_command_worker_start(false)); vkCmdDraw = saved;
    vkCmdBeginRenderingKHR = vkCmdBeginRendering; assert(vkCmdBeginRenderingKHR == rendering);
    for (unsigned f = 0; f < 16; f++) {
        failing_alloc = true; fail_index = f; alloc_index = 0;
        assert(!vk_command_worker_start(false) && !vk_command_worker_active() && !live_allocs);
        failing_alloc = false;
    }
    failing_alloc = true; fail_index = 99; alloc_index = 0; failing_thread = true;
    assert(!vk_command_worker_start(false) && !live_allocs && !vk_command_worker_active());
    failing_alloc = failing_thread = false;
    assert(vk_command_worker_start(true)); snapshots();
    phase = 0;
    vkCmdBindPipeline(HANDLE(VkCommandBuffer, 1), VK_PIPELINE_BIND_POINT_GRAPHICS, HANDLE(VkPipeline, 2));
    uint8_t *large = malloc(70000); memset(large, 0x33, 70000);
    vkCmdPushConstants(HANDLE(VkCommandBuffer, 1), HANDLE(VkPipelineLayout, 2), VK_SHADER_STAGE_VERTEX_BIT, 32, 70000, large); free(large);
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .pNext = &m};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = &m};
    vkCmdBindPipeline(HANDLE(VkCommandBuffer, 1), VK_PIPELINE_BIND_POINT_GRAPHICS, HANDLE(VkPipeline, 2));
    vkCmdPipelineBarrier2(HANDLE(VkCommandBuffer, 1), &dep);
    dep.pNext = NULL; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &m;
    vkCmdBindPipeline(HANDLE(VkCommandBuffer, 1), VK_PIPELINE_BIND_POINT_GRAPHICS, HANDLE(VkPipeline, 2));
    vkCmdPipelineBarrier2(HANDLE(VkCommandBuffer, 1), &dep);
    VkCommandWorkerStats stats = vk_command_worker_stats();
    assert(stats.fallback == 3 && stats.measured && stats.cpu_ns && stats.flush_wait_ns);
    vk_command_worker_stop(); assert(!vk_command_worker_active());
    // Hold a real driver call, fill >1 MiB, and prove that the producer waits.
    assert(vk_command_worker_start(false)); phase = 2; gate_command(); atomic_store(&stress, true);
    for (uint32_t i = 0; i < 40000; i++) vkCmdDraw(HANDLE(VkCommandBuffer, 1), 3, 1, i, 0);
    atomic_store(&stress, false); assert(atomic_load(&backpressure));
    assert(vkResetCommandPool(HANDLE(VkDevice, 1), HANDLE(VkCommandPool, 2), 0) == VK_ERROR_OUT_OF_HOST_MEMORY);
    for (uint32_t i = 40000; i < 40003; i++) vkCmdDraw(HANDLE(VkCommandBuffer, 1), 3, 1, i, 0);
    vk_command_worker_stop(); assert(draw_index == 40003); // stop publishes the final partial slot
    assert(vk_command_worker_stats().commands == 40004);
    assert(vk_command_worker_start(false)); phase = 0;
    vkCmdBindPipeline(HANDLE(VkCommandBuffer, 1), VK_PIPELINE_BIND_POINT_GRAPHICS, HANDLE(VkPipeline, 2));
    vk_command_worker_stop(); assert(vk_command_worker_stats().commands == 1);
    puts("PASS: deep snapshots, ordered fallback, queue-full backpressure, slot reuse, drain/stop/restart, allocation/thread failures, VkResult and bare PFNs");
}
