// Experimental second-stage Vulkan recording. A single producer snapshots every pointer parameter;
// command buffers and resources keep their existing submission lifetimes. GPU completion is still separate.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

#ifdef __cplusplus
extern "C" {
#endif
#define CW_HIDDEN __attribute__((visibility("hidden")))
typedef struct {
    uint64_t commands, batches, fallback, cpu_ns, flush_wait_ns;
    bool measured;
} VkCommandWorkerStats;
// Called on the render producer after the device is ready. Start/flush/stop share that owner.
CW_HIDDEN bool vk_command_worker_start(bool measure);
CW_HIDDEN bool vk_command_worker_active(void);
CW_HIDDEN void vk_command_worker_flush(void);
CW_HIDDEN void vk_command_worker_stop(void);
CW_HIDDEN VkCommandWorkerStats vk_command_worker_stats(void);

// Function-like macros leave bare PFN availability checks and compatibility aliases unchanged.
#include "vk_command_worker_decl.h"
#ifdef __cplusplus
}
#endif
