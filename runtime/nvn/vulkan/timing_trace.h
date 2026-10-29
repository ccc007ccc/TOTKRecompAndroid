#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../diagnostics_build.h"

#ifdef __ANDROID__
enum { VK_TIMING_TIMELINE, VK_TIMING_SLOT, VK_TIMING_ACQUIRE, VK_TIMING_PRESENT, VK_TIMING_SUBMIT, VK_TIMING_POLL,
       VK_TIMING_FRAME_LIMIT, VK_TIMING_TRANSLATION, VK_TIMING_DRAIN, VK_TIMING_QUEUE,
       VK_TIMING_TEXTURE_UNCHANGED, VK_TIMING_TEXTURE_DRAIN, VK_TIMING_SAMPLER_DRAIN, VK_TIMING_TEXTURE_QUEUED, VK_TIMING_WAITS };
typedef struct {
    uint64_t address, frame, seq, host_ns;
    int64_t raw_ns, idle_ns;
    uint64_t gpu_ticks, delivered_ticks;
    bool idle_matched;
} VkTimingCounter;
#if TOTK_DIAGNOSTICS
bool vk_timing_trace_on(void);
void vk_timing_trace_wait(int kind, uint64_t ns);
void vk_timing_trace_counter(const VkTimingCounter *sample);
void vk_timing_trace_submission(uint64_t ns, bool presentation);
void vk_timing_trace_fence(uint64_t requested, uint64_t submitted);
void vk_timing_trace_query_failure(int result, unsigned counters, uint64_t seq);
void vk_timing_trace_frame(uint64_t frame);
#else
static inline bool vk_timing_trace_on(void) { return false; }
static inline void vk_timing_trace_wait(int kind, uint64_t ns) { (void)kind; (void)ns; }
static inline void vk_timing_trace_counter(const VkTimingCounter *sample) { (void)sample; }
static inline void vk_timing_trace_submission(uint64_t ns, bool presentation) { (void)ns; (void)presentation; }
static inline void vk_timing_trace_fence(uint64_t requested, uint64_t submitted) { (void)requested; (void)submitted; }
static inline void vk_timing_trace_query_failure(int result, unsigned counters, uint64_t seq) { (void)result; (void)counters; (void)seq; }
static inline void vk_timing_trace_frame(uint64_t frame) { (void)frame; }
#endif
#endif
