// Shader compilation and reflection for the Vulkan backend (shaders.c).
#pragma once
#include <stddef.h>

#include "vk.h"

typedef struct { uint32_t set, binding; VkDescriptorType type; } VkBindingInfo;

void vk_shaders_init(void);
// Source identifies the exact prepared variant. This function never compiles;
// missing/corrupt output stops execution instead of silently omitting a draw.
uint32_t *vk_load_prepared_shader(const char *src, int stage, const char *name, size_t *nwords);
VkShaderModule vk_module(const uint32_t *spv, size_t nwords);
// Descriptor bindings declared by a SPIR-V module.
int vk_reflect(const uint32_t *spv, size_t nwords, VkBindingInfo *out, int max);
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages);
VkResult vk_try_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages, VkDescriptorSetLayout *layout);
