#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define VK_COMMAND_WORKER_IMPLEMENTATION 1
#include "command_worker.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// One producer, one consumer. Publication and retirement protect a slot's bytes; no Vulkan work is dropped.
enum { CW_SLOTS = 16, CW_BYTES = 65536, CW_PUBLISH_BYTES = 4096 };
typedef struct { uint32_t op, size; uint64_t reserved; } CWHeader;
typedef struct { void *data; size_t used; } CWNode;
static CWNode nodes[CW_SLOTS];
static size_t used;
static uint64_t head;
static _Atomic uint64_t published, completed;
static _Atomic bool active;
static bool stopping;
static _Atomic bool measure;
static pthread_t owner, worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t more = PTHREAD_COND_INITIALIZER, less = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t commands, batches, fallbacks, cpu_ns, flush_wait_ns;
static void cw_execute(void *data);

static uint64_t cw_clock(clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static bool cw_owned(void) {
    return atomic_load_explicit(&active, memory_order_acquire) && pthread_equal(pthread_self(), owner);
}
bool vk_command_worker_active(void) { return atomic_load_explicit(&active, memory_order_acquire); }
static size_t cw_align(size_t n) { return (n + 15) & ~(size_t)15; }

static void *cw_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "larecomp-vkw");
    uint64_t tail = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (tail == atomic_load_explicit(&published, memory_order_acquire) && !stopping)
            pthread_cond_wait(&more, &lock);
        bool end = stopping && tail == atomic_load_explicit(&published, memory_order_acquire);
        pthread_mutex_unlock(&lock);
        if (end) break;
        CWNode *node = &nodes[tail % CW_SLOTS];
        uint64_t start = measure ? cw_clock(CLOCK_THREAD_CPUTIME_ID) : 0;
        uint64_t count = 0;
        for (size_t off = 0; off < node->used;) {
            CWHeader h; memcpy(&h, (uint8_t *)node->data + off, sizeof h);
            if (h.size < sizeof h || h.size > node->used - off || (h.size & 15)) abort();
            cw_execute((uint8_t *)node->data + off);
            off += h.size; count++;
        }
        if (measure) atomic_fetch_add_explicit(&cpu_ns, cw_clock(CLOCK_THREAD_CPUTIME_ID) - start, memory_order_relaxed);
        atomic_fetch_add_explicit(&commands, count, memory_order_relaxed);
        atomic_fetch_add_explicit(&batches, 1, memory_order_relaxed);
        node->used = 0;
        atomic_store_explicit(&completed, ++tail, memory_order_release);
        pthread_mutex_lock(&lock);
        pthread_cond_broadcast(&less);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

bool vk_command_worker_start(bool timed) {
    if (vk_command_worker_active()) return cw_owned();
    // All Vulkan PFNs must already belong to the current device. No loader pointers are replaced.
    if (!vkCmdBindPipeline || !vkCmdDraw || !vkCmdBindDescriptorSets || !vkEndCommandBuffer) return false;
    for (unsigned i = 0; i < CW_SLOTS; i++) {
        nodes[i].data = malloc(CW_BYTES);
        nodes[i].used = 0;
        if (!nodes[i].data) {
            for (unsigned j = 0; j < i; j++) { free(nodes[j].data); nodes[j].data = NULL; }
            return false;
        }
    }
    owner = pthread_self(); measure = timed; stopping = false; head = used = 0;
    atomic_store(&published, 0); atomic_store(&completed, 0);
    atomic_store(&commands, 0); atomic_store(&batches, 0); atomic_store(&fallbacks, 0);
    atomic_store(&cpu_ns, 0); atomic_store(&flush_wait_ns, 0);
    if (pthread_create(&worker, NULL, cw_main, NULL)) {
        for (unsigned i = 0; i < CW_SLOTS; i++) { free(nodes[i].data); nodes[i].data = NULL; }
        return false;
    }
    atomic_store_explicit(&active, true, memory_order_release);
    return true;
}

static void cw_publish(void) {
    if (!used) return;
    nodes[head % CW_SLOTS].used = used;
    atomic_store_explicit(&published, ++head, memory_order_release);
    used = 0;
    pthread_mutex_lock(&lock);
    pthread_cond_signal(&more);
    pthread_mutex_unlock(&lock);
}
static void *cw_reserve(size_t size) {
    if (size > CW_BYTES) return NULL;
    size = cw_align(size);
    if (used + size > CW_BYTES) cw_publish();
    if (!used && head - atomic_load_explicit(&completed, memory_order_acquire) >= CW_SLOTS) {
        pthread_mutex_lock(&lock);
        while (head - atomic_load_explicit(&completed, memory_order_acquire) >= CW_SLOTS)
            pthread_cond_wait(&less, &lock);
        pthread_mutex_unlock(&lock);
    }
    return (uint8_t *)nodes[head % CW_SLOTS].data + used;
}
static void cw_finish(size_t size) {
    used += size;
    if (used >= CW_PUBLISH_BYTES) cw_publish();
}
void vk_command_worker_flush(void) {
    // Foreign-thread handoff goes through an ordered render-queue callback; it may not publish producer bytes.
    if (!cw_owned()) return;
    if (!used && atomic_load_explicit(&completed, memory_order_acquire) == head) return;
    uint64_t start = measure ? cw_clock(CLOCK_MONOTONIC) : 0;
    cw_publish();
    pthread_mutex_lock(&lock);
    while (atomic_load_explicit(&completed, memory_order_acquire) < head)
        pthread_cond_wait(&less, &lock);
    pthread_mutex_unlock(&lock);
    if (measure) atomic_fetch_add_explicit(&flush_wait_ns, cw_clock(CLOCK_MONOTONIC) - start, memory_order_relaxed);
}
void vk_command_worker_stop(void) {
    if (!cw_owned()) return;
    vk_command_worker_flush();
    pthread_mutex_lock(&lock); stopping = true; pthread_cond_signal(&more); pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    atomic_store_explicit(&active, false, memory_order_release);
    for (unsigned i = 0; i < CW_SLOTS; i++) { free(nodes[i].data); nodes[i].data = NULL; }
    used = head = 0;
}
VkCommandWorkerStats vk_command_worker_stats(void) {
    return (VkCommandWorkerStats){atomic_load(&commands), atomic_load(&batches), atomic_load(&fallbacks),
                                 atomic_load(&cpu_ns), atomic_load(&flush_wait_ns), atomic_load(&measure)};
}
static void cw_fallback(void) {
    atomic_fetch_add_explicit(&fallbacks, 1, memory_order_relaxed);
    vk_command_worker_flush();
}

// Pointers in a packet are offsets until the consumer owns it. Overflow, oversized commands and unknown pNext
// chains run synchronously after a drain, preserving the caller's original Vulkan semantics.
static void *cw_pointer(void *base, const void *offset) {
    return offset ? (uint8_t *)base + (uintptr_t)offset : NULL;
}
static bool cw_array_size(size_t *size, const void *p, size_t count, size_t stride) {
    if (count && (!p || count > SIZE_MAX / stride)) return false;
    size_t n = count * stride;
    if (*size > CW_BYTES || n > CW_BYTES - cw_align(*size)) return false;
    if (n) *size = cw_align(*size) + n;
    return true;
}
static void *cw_copy(void *base, size_t *cursor, const void *p, size_t n) {
    if (!n) return NULL;
    *cursor = cw_align(*cursor);
    size_t off = *cursor;
    memcpy((uint8_t *)base + off, p, n);
    *cursor += n;
    return (void *)(uintptr_t)off;
}
#define CW_PLAIN(T) \
    static bool cw_plain_##T(const T *p, size_t n) { \
        if (n && !p) return false; \
        for (size_t i = 0; i < n; i++) if (p[i].pNext) return false; \
        return true; \
    }
CW_PLAIN(VkMemoryBarrier) CW_PLAIN(VkBufferMemoryBarrier) CW_PLAIN(VkImageMemoryBarrier)
CW_PLAIN(VkMemoryBarrier2) CW_PLAIN(VkBufferMemoryBarrier2) CW_PLAIN(VkImageMemoryBarrier2)
CW_PLAIN(VkRenderingAttachmentInfo)
#undef CW_PLAIN

static bool cw_size_rendering(size_t *n, const VkRenderingInfo *p) {
    if (!p || p->pNext || !cw_array_size(n, p, 1, sizeof *p)) return false;
    if (!cw_array_size(n, p->pColorAttachments, p->colorAttachmentCount, sizeof *p->pColorAttachments) ||
        !cw_plain_VkRenderingAttachmentInfo(p->pColorAttachments, p->colorAttachmentCount)) return false;
    if (p->pDepthAttachment && (p->pDepthAttachment->pNext || !cw_array_size(n, p->pDepthAttachment, 1, sizeof *p->pDepthAttachment))) return false;
    if (p->pStencilAttachment && (p->pStencilAttachment->pNext || !cw_array_size(n, p->pStencilAttachment, 1, sizeof *p->pStencilAttachment))) return false;
    return true;
}
static void *cw_copy_rendering(void *data, size_t *cursor, const VkRenderingInfo *p) {
    void *off = cw_copy(data, cursor, p, sizeof *p);
    VkRenderingInfo *q = cw_pointer(data, off);
    q->pColorAttachments = cw_copy(data, cursor, p->pColorAttachments, p->colorAttachmentCount * sizeof *p->pColorAttachments);
    q->pDepthAttachment = p->pDepthAttachment ? cw_copy(data, cursor, p->pDepthAttachment, sizeof *p->pDepthAttachment) : NULL;
    q->pStencilAttachment = p->pStencilAttachment ? cw_copy(data, cursor, p->pStencilAttachment, sizeof *p->pStencilAttachment) : NULL;
    return off;
}
static void cw_fix_rendering(VkRenderingInfo *p, void *data) {
    p->pColorAttachments = cw_pointer(data, p->pColorAttachments);
    p->pDepthAttachment = cw_pointer(data, p->pDepthAttachment);
    p->pStencilAttachment = cw_pointer(data, p->pStencilAttachment);
}
static bool cw_size_dependency(size_t *n, const VkDependencyInfo *p) {
    return p && !p->pNext && cw_array_size(n, p, 1, sizeof *p) &&
        cw_array_size(n, p->pMemoryBarriers, p->memoryBarrierCount, sizeof *p->pMemoryBarriers) && cw_plain_VkMemoryBarrier2(p->pMemoryBarriers, p->memoryBarrierCount) &&
        cw_array_size(n, p->pBufferMemoryBarriers, p->bufferMemoryBarrierCount, sizeof *p->pBufferMemoryBarriers) && cw_plain_VkBufferMemoryBarrier2(p->pBufferMemoryBarriers, p->bufferMemoryBarrierCount) &&
        cw_array_size(n, p->pImageMemoryBarriers, p->imageMemoryBarrierCount, sizeof *p->pImageMemoryBarriers) && cw_plain_VkImageMemoryBarrier2(p->pImageMemoryBarriers, p->imageMemoryBarrierCount);
}
static void *cw_copy_dependency(void *data, size_t *cursor, const VkDependencyInfo *p) {
    void *off = cw_copy(data, cursor, p, sizeof *p);
    VkDependencyInfo *q = cw_pointer(data, off);
    q->pMemoryBarriers = cw_copy(data, cursor, p->pMemoryBarriers, p->memoryBarrierCount * sizeof *p->pMemoryBarriers);
    q->pBufferMemoryBarriers = cw_copy(data, cursor, p->pBufferMemoryBarriers, p->bufferMemoryBarrierCount * sizeof *p->pBufferMemoryBarriers);
    q->pImageMemoryBarriers = cw_copy(data, cursor, p->pImageMemoryBarriers, p->imageMemoryBarrierCount * sizeof *p->pImageMemoryBarriers);
    return off;
}
static void cw_fix_dependency(VkDependencyInfo *p, void *data) {
    p->pMemoryBarriers = cw_pointer(data, p->pMemoryBarriers);
    p->pBufferMemoryBarriers = cw_pointer(data, p->pBufferMemoryBarriers);
    p->pImageMemoryBarriers = cw_pointer(data, p->pImageMemoryBarriers);
}
static bool cw_size_renderpass(size_t *n, const VkRenderPassBeginInfo *p) {
    return p && !p->pNext && cw_array_size(n, p, 1, sizeof *p) &&
        cw_array_size(n, p->pClearValues, p->clearValueCount, sizeof *p->pClearValues);
}
static void *cw_copy_renderpass(void *data, size_t *cursor, const VkRenderPassBeginInfo *p) {
    void *off = cw_copy(data, cursor, p, sizeof *p);
    VkRenderPassBeginInfo *q = cw_pointer(data, off);
    q->pClearValues = cw_copy(data, cursor, p->pClearValues, p->clearValueCount * sizeof *p->pClearValues);
    return off;
}
static void cw_fix_renderpass(VkRenderPassBeginInfo *p, void *data) {
    p->pClearValues = cw_pointer(data, p->pClearValues);
}

#include "vk_command_worker_packets.inc"
