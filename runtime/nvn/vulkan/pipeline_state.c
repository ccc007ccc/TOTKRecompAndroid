// Shared by live draws and recorded-pipeline warming. Keep one Vulkan builder.
#include "pipeline_state.h"
#include "compile_budget.h"

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

bool vk_pipeline_state_valid(const VkPipelineState *k) {
    if (!k || k->ncolor > 8 || !k->samples || k->samples > 64 || (k->samples & (k->samples-1)) ||
        (k->used_streams >> 17) || k->topo_class > 2 || k->fx.on > 1 || k->a2c > 1 ||
        k->poly_fill > 1 || k->depth_clamp > 1 || k->prim_restart > 1) return false;
    for (unsigned i=0; i<32; ++i) if ((k->attr_mask & (1u<<i)) && k->attr_bind[i] >= 17) return false;
    for(unsigned i=0;i<8;++i)if(k->write_mask[i]>15)return false;
    if(k->fx.cull>3 || k->fx.ff>1 || k->fx.topo>VK_PRIMITIVE_TOPOLOGY_PATCH_LIST || k->fx.dt>1 || k->fx.dw>1 || k->fx.sten>1 || k->fx.dop>VK_COMPARE_OP_ALWAYS)return false;
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j)if(k->fx.sop[i][j]>7)return false;
    return true;
}
bool vk_pipeline_state_supported(const VkPipelineState *s) {
    if(!vk_pipeline_state_valid(s))return false;
    if(!g_vk.phys)return true; // CPU codec tests have no physical device
    const VkPhysicalDeviceLimits *l=&g_vk.props.limits;
    if(s->ncolor>l->maxColorAttachments)return false;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(g_vk.phys,&features);
    if((s->depth_clamp && !features.depthClamp) || (!s->poly_fill && !features.fillModeNonSolid))return false;
    VkSampleCountFlags samples=l->framebufferColorSampleCounts;
    if(s->depth)samples&=l->framebufferDepthSampleCounts;if(s->stencil)samples&=l->framebufferStencilSampleCounts;
    if(!(samples&s->samples))return false;
    unsigned attributes=0,bindings=0;
    for(unsigned i=0;i<32;++i)if(s->attr_mask&(1u<<i)) {
        ++attributes;if(i>=l->maxVertexInputAttributes || s->attr_bind[i]>=l->maxVertexInputBindings || s->attr_off[i]>l->maxVertexInputAttributeOffset || s->attr_fmt[i]>VK_FORMAT_ASTC_12x12_SRGB_BLOCK)return false;
        VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(g_vk.phys,s->attr_fmt[i],&properties);
        if(!(properties.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))return false;
    }
    for(unsigned i=0;i<17;++i)if(s->used_streams&(1u<<i)){++bindings;if(i>=l->maxVertexInputBindings || s->stride[i]>l->maxVertexInputBindingStride)return false;}
    if(attributes>l->maxVertexInputAttributes || bindings>l->maxVertexInputBindings)return false;
    for(unsigned i=0;i<s->ncolor;++i)if(s->color[i]) {
        if(s->color[i]>VK_FORMAT_ASTC_12x12_SRGB_BLOCK)return false;
        VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(g_vk.phys,s->color[i],&properties);
        if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))return false;
    }
    for(unsigned i=0;i<2;++i)if(i?s->stencil:s->depth) {
        if((i?s->stencil:s->depth)>VK_FORMAT_ASTC_12x12_SRGB_BLOCK)return false;
        VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(g_vk.phys,i?s->stencil:s->depth,&properties);
        if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))return false;
    }
    return true;
}
bool vk_pipeline_local_size_supported(const uint32_t local[3]) {
    if(!vk_pipeline_local_size_valid(local))return false;
    const VkPhysicalDeviceLimits *l=&g_vk.props.limits;if(!g_vk.phys)return true;
    for(unsigned i=0;i<3;++i)if(local[i]>l->maxComputeWorkGroupSize[i])return false;
    return (uint64_t)local[0]*local[1]*local[2]<=l->maxComputeWorkGroupInvocations;
}
bool vk_pipeline_local_size_valid(const uint32_t local[3]) {
    return local && local[0] && local[1] && local[2] && local[0]<=1024 && local[1]<=1024 && local[2]<=1024 &&
        (uint64_t)local[0]*local[1]*local[2]<=1024;
}
VkResult vk_pipeline_compute_build(VkShaderModule module,VkPipelineLayout layout,const uint32_t local[3],VkPipeline *pipeline) {
    *pipeline=VK_NULL_HANDLE;if(!vk_pipeline_local_size_valid(local))return VK_ERROR_INITIALIZATION_FAILED;
    VkSpecializationMapEntry entries[3]={{0,0,4},{1,4,4},{2,8,4}};
    VkSpecializationInfo spec={3,entries,12,local};
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=module,.pName="main",.pSpecializationInfo=&spec},.layout=layout};
    if(!vk_compile_budget_acquire())return VK_NOT_READY;
    VkResult result=vkCreateComputePipelines(g_vk.dev,g_vk.pcache,1,&ci,NULL,pipeline);
    vk_compile_budget_release();return result;
}
VkResult vk_pipeline_state_build(const VkPipelineState *k, VkShaderModule vertex, VkShaderModule fragment,
    VkPipelineLayout layout, uint32_t dynamic_count, const VkDynamicState *dynamic_states,
    VkPipelineCreateFlags flags, VkPipeline *pipeline) {
    *pipeline = VK_NULL_HANDLE;
    if (!vk_pipeline_state_valid(k)) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class), .primitiveRestartEnable = k->prim_restart};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    // (no extended dynamic state 2: depth bias always on, "off" being a bias of zero - draw_ sets it either way)
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1,
                                                 .depthClampEnable = k->depth_clamp, .depthBiasEnable = !g_vk.eds2};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = dynamic_count, .pDynamicStates = dynamic_states};
    if (k->fx.on) {  // (no extended dynamic state: the draw's own, PipeFx)
        const PipeFx *x = &k->fx;
        ia.topology = (VkPrimitiveTopology)x->topo;
        rs.cullMode = x->cull;
        rs.frontFace = (VkFrontFace)x->ff;
        ds.depthTestEnable = x->dt;
        ds.depthWriteEnable = x->dw;
        ds.depthCompareOp = (VkCompareOp)x->dop;
        ds.stencilTestEnable = x->sten;
        ds.front = (VkStencilOpState){(VkStencilOp)x->sop[0][0], (VkStencilOp)x->sop[0][1], (VkStencilOp)x->sop[0][2], (VkCompareOp)x->sop[0][3]};
        ds.back = (VkStencilOpState){(VkStencilOp)x->sop[1][0], (VkStencilOp)x->sop[1][1], (VkStencilOp)x->sop[1][2], (VkCompareOp)x->sop[1][3]};
    }
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout};
    {   // (no dynamic rendering: against a compatible render pass, rpcompat.c)
        VkRenderPass crp = vk_rp_compat(k->ncolor, k->color, k->depth, k->stencil, ms.rasterizationSamples);
        if (crp) pi.pNext = ri.pNext, pi.renderPass = crp;
    }
    pi.flags = flags;
    if(!vk_compile_budget_acquire())return VK_NOT_READY;
    VkResult result=vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, pipeline);
    vk_compile_budget_release();return result;
}
