// Actual compiler target and complete source-key contract. Keep this separate
// from loader, scheduling and persistence code so mappings track key semantics.
#pragma once
#include "shaders.h"
#include "shader_cache.h"
#include <stdlib.h>
static inline uint32_t vk_shader_target_api(void) {
#ifdef __ANDROID__
    return g_vk.api?g_vk.api:VK_API_VERSION_1_3;
#else
    return VK_API_VERSION_1_3;
#endif
}
static inline uint64_t vk_shader_source_key(const char *source,int stage) {
    return vk_shader_source_hash(source,stage,getenv("LARECOMP_VK_OPT")!=NULL)^((uint64_t)vk_shader_target_api()<<32);
}
