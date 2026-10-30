// Device-local immutable SPIR-V, reflection and shader-module reuse. Leases
// protect both building and ready entries; layouts remain program-owned.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "shaders.h"
typedef struct {
    void *entry;
    VkShaderModule module;
    const uint32_t *spirv; // may be NULL for transient (unretained) leases
    size_t words;
    const VkBindingInfo *bindings;
    unsigned count;
} VkShaderModuleLease;
typedef struct { uint64_t hits, builds; unsigned entries; size_t bytes; } VkShaderModuleCacheStats;
// Optional supplied words are already validated output from the exact source
// cache or compiler. They remain caller-owned; an entry retains its own copy.
bool vk_shader_module_acquire(uint64_t key, int stage, const uint32_t *words, size_t count, VkShaderModuleLease *out);
void vk_shader_module_release(VkShaderModuleLease *lease);
// Called after warm/compile jobs and GPU use have drained and programs release
// their leases. A pinned entry is detached, never destroyed under a user.
void vk_shader_module_cache_reset(void);
VkShaderModuleCacheStats vk_shader_module_cache_stats(void);
#ifdef __cplusplus
}
#endif
