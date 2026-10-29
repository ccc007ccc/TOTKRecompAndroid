// Handle-independent Vulkan graphics state, captured by value for builds and warm recipes.
#pragma once
#include "vk.h"

typedef struct {
    uint8_t on, cull, ff, topo, dt, dw, dop, sten;
    uint8_t sop[2][4];  // per face: fail, pass, depth fail, compare
} PipeFx;

typedef struct {
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp;
    uint8_t prim_restart;  // (no extended dynamic state 2 only: primitive restart is pipeline state then)
    uint32_t write_mask[8];
    PipeFx fx;
} VkPipelineState;

bool vk_pipeline_state_valid(const VkPipelineState *state);
VkResult vk_pipeline_state_build(const VkPipelineState *state, VkShaderModule vertex, VkShaderModule fragment,
    VkPipelineLayout layout, uint32_t dynamic_count, const VkDynamicState *dynamic_states,
    VkPipelineCreateFlags flags, VkPipeline *pipeline);
bool vk_pipeline_local_size_valid(const uint32_t local[3]);
VkResult vk_pipeline_compute_build(VkShaderModule module, VkPipelineLayout layout, const uint32_t local[3], VkPipeline *pipeline);
