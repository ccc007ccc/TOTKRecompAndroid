// GPU conversion of Tegra block-linear uploads. The source remains compressed for BC/ASTC images: the texture
// units decode it when sampled. This pass only rearranges the bytes, using the layout from tegra.c.
#include "vk.h"
#include "deswizzle_shader.h"
#include <stdlib.h>

static int state = -1;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkPipeline pipe;

// Called with submission/translation drained and the GPU idle (vk_textures_reset).
void vk_deswizzle_reset(void) {
    if (pipe) vkDestroyPipeline(g_vk.dev, pipe, NULL);
    if (layout) vkDestroyPipelineLayout(g_vk.dev, layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(g_vk.dev, set_layout, NULL);
    pipe = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    state = -1;
}

static bool prepare(void) {
    if (state >= 0) return state != 0;
    const char *e = getenv("LARECOMP_VK_GPU_DETILE");
    state = 0;
    // Opt in while the device/performance acceptance is pending.
    if (!e || !atoi(e)) return false;
    VkDescriptorSetLayoutBinding binds[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = binds};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkResult result = vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout);
    if (result == VK_SUCCESS) result = vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &layout);
    VkShaderModule shader = result == VK_SUCCESS ? vk_builtin_shader("deswizzle.comp") : VK_NULL_HANDLE;
    if (result == VK_SUCCESS && !shader) result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
            .layout = layout};
        result = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe);
    }
    if (result != VK_SUCCESS) {
        vk_deswizzle_reset();
        state = 0;
        LOG("[vk] GPU texture de-tiling unavailable (%d); using CPU uploads\n", result);
        return false;
    }
    state = 1;
    LOG("[vk] GPU texture de-tiling active (compressed BC/ASTC blocks preserved)\n");
    return true;
}

bool vk_deswizzle_upload(VkCommandBuffer cb, const void *src, uint64_t available, uint32_t row, uint32_t hb,
                         uint32_t depth, uint32_t bh, uint32_t bd, VkSpan *dst) {
    uint32_t pc[8];
    uint64_t bytes;
    const VkPhysicalDeviceLimits *limits = &g_vk.props.limits;
    if (!src || !dst || !vk_deswizzle_params(row, hb, depth, bh, bd, 0, pc, &bytes) || bytes > available ||
        bytes > limits->maxStorageBufferRange || (uint64_t)pc[3] * 4 > limits->maxStorageBufferRange ||
        limits->maxComputeWorkGroupSize[0] < 64 || limits->maxComputeWorkGroupInvocations < 64 ||
        !limits->maxComputeWorkGroupCount[0] || !prepare()) return false;

    uint64_t align = limits->minStorageBufferOffsetAlignment;
    if (align < 16) align = 16;
    // Preserve the original CPU upload's point-in-time contents. Reading the guest buffer later from the GPU
    // would race WriteTexels, pool rewrites and reuse. The transient ring keeps both spans until GPU completion;
    // this sequential snapshot copy does no de-tiling or BC/ASTC decoding and works without a shared guest map.
    VkSpan source = vk_transient_copy(src, bytes, align);
    *dst = vk_transient((uint64_t)pc[3] * 4, align);
    VkDescriptorSet set = vk_alloc_set(set_layout);
    VkDescriptorBufferInfo bi[] = {{source.buf, source.off, bytes}, {dst->buf, dst->off, (uint64_t)pc[3] * 4}};
    VkWriteDescriptorSet writes[2];
    for (uint32_t i = 0; i < 2; ++i)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    uint32_t groups = (pc[3] + 63) / 64;
    if (groups > 4096) groups = 4096;
    if (groups > limits->maxComputeWorkGroupCount[0]) groups = limits->maxComputeWorkGroupCount[0];
    vkCmdDispatch(cb, groups, 1, 1);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cb, &dep);
    return true;
}
