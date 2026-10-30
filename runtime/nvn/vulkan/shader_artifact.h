// Prepared CPU interface metadata paired with the existing validated SPIR-V
// cache. No generated source, guest address, pointer or Vulkan handle is stored.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "../maxwell/shader.h"
typedef struct {
    ShaderInfo *info;  // restored metadata has msl == NULL
    uint32_t *spirv;
    size_t words;
    uint64_t source_key;
    bool vertex_index;
} VkShaderArtifact;
typedef struct { uint64_t hits, misses, stored, failed; } VkShaderArtifactStats;
// Same complete fixed-control + NVN code/constants hash as the shared-list key.
// data starts at the translator's input; compute retains its NVN prefix too.
bool vk_shader_artifact_input_key(const void *control, size_t control_bytes,
                                 const void *prefix, size_t prefix_bytes,
                                 const void *data, size_t data_bytes, uint8_t key[32]);
// Called under the translator lock after configuring the current globals.
uint64_t vk_shader_artifact_profile(uint64_t renderer_profile);
bool vk_shader_artifact_source_required(void);
// True means accepted for bounded publication. Disk failure remains optional
// and is counted separately; only a successful load proves artifact availability.
bool vk_shader_artifact_store(const uint8_t raw[32], const ShaderKey *key, uint64_t profile, const ShaderInfo *info);
bool vk_shader_artifact_load(const uint8_t raw[32], const ShaderKey *key, uint64_t profile, int stage, VkShaderArtifact *out);
void vk_shader_artifact_free(VkShaderArtifact *artifact);
// Drain optional metadata publications before receipts/reset. SPIR-V remains in
// the existing compiler cache; queue saturation/write failure keeps fallback.
void vk_shader_artifact_flush(void);
VkShaderArtifactStats vk_shader_artifact_stats(void);
#ifdef __cplusplus
}
#endif
