#pragma once
#include "pipeline_state.h"

// shader[1]==0 denotes compute; then only shader[0] and local[] are populated.
typedef struct { uint64_t shader[2]; VkPipelineState state; uint32_t local[3]; } VkPipelineRecipe;
// Keep creation layouts alive with the pipeline, including on Vulkan devices
// without maintenance4. No handles from this object are ever persisted.
typedef struct { VkPipeline pipeline; VkPipelineLayout layout; VkDescriptorSetLayout sets[2]; } VkPipelineWarmProduct;
typedef VkResult (*VkPipelineWarmBuild)(const VkPipelineRecipe *, VkPipelineWarmProduct *);
typedef struct { uint32_t loaded, prepared, used, missing, failed, cancelled; } VkPipelineWarmStats;

void vk_pipeline_recipe_make(VkPipelineRecipe *out, uint64_t vertex, uint64_t fragment, const VkPipelineState *state);
void vk_pipeline_recipe_compute(VkPipelineRecipe *out, uint64_t compute, const uint32_t local[3]);
void vk_pipeline_warm_start(VkPipelineWarmBuild build);
void vk_pipeline_warm_stop(void);
void vk_pipeline_warm_record(const VkPipelineRecipe *recipe);
bool vk_pipeline_warm_take(const VkPipelineRecipe *recipe, VkPipelineWarmProduct *product);
void vk_pipeline_warm_discard(VkPipelineWarmProduct *product);
bool vk_pipeline_warm_save(void);
VkPipelineWarmStats vk_pipeline_warm_stats(void);
// The renderer provides this entry, using the same module/layout/pipeline builder.
void vk_pipeline_warm_begin(void);
