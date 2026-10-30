// Portable fixed-function recipes, rebuilt on the current Vulkan device. No
// driver binaries, pointers, guest addresses, or native struct padding on disk.
#pragma once
#include "pipeline_warm.h"
typedef struct { char translation[65], recipe[65], compiler[65]; uint32_t game, features; } VkPipelineSeedIdentity;
#ifdef __cplusplus
extern "C" {
#endif
bool vk_pipeline_seed_save(const char *path,const VkPipelineSeedIdentity *identity,const VkPipelineRecipe *recipes,unsigned count);
unsigned vk_pipeline_seed_load(const char *path,const VkPipelineSeedIdentity *identity,VkPipelineRecipe *recipes,unsigned capacity);
VkPipelineSeedIdentity vk_pipeline_seed_identity(unsigned game);
#ifdef __cplusplus
}
#endif
