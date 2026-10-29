// Execute TLDS.MS fragments emitted by test_shader_multisample.c on a real GPU.
// Input texels encode x/y/sample/channel independently, so sample/LOD confusion,
// coordinate scaling and incorrect F16 register packing cannot pass by accident.
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r); exit(1); } } while (0)
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static unsigned messages;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { ++messages; fprintf(stderr, "%s\n", data->pMessage); }
    return VK_FALSE;
}
static VkDeviceMemory memory(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties; vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (unsigned i = 0; i < properties.memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (1u << i)) || (properties.memoryTypes[i].propertyFlags & flags) != flags) continue;
        VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = requirements.size, .memoryTypeIndex = i};
        VkDeviceMemory result; CHECK(vkAllocateMemory(device, &info, NULL, &result)); return result;
    }
    fprintf(stderr, "No suitable memory type\n"); exit(1);
}
static VkShaderModule module(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file); fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    assert(size > 0 && !(size % 4)); uint32_t *words = malloc(size); assert(words);
    assert(fread(words, 1, size, file) == (size_t)size); fclose(file);
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = words};
    VkShaderModule result; CHECK(vkCreateShaderModule(device, &mi, NULL, &result)); free(words); return result;
}
static VkPipeline pipeline(const char *path, VkPipelineLayout layout) {
    VkShaderModule shader = module(path);
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader, .pName = "main"}, .layout = layout};
    VkPipeline result; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &result));
    vkDestroyShaderModule(device, shader, NULL); return result;
}
static VkPipeline fill_pipeline(const char *vertex, const char *fragment, VkPipelineLayout layout, VkRenderPass pass) {
    VkShaderModule vs = module(vertex), fs = module(fragment);
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo inputs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, 4, 4, 0, 1}; VkRect2D scissor = {{0, 0}, {4, 4}};
    VkPipelineViewportStateCreateInfo viewports = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
        .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT, .sampleShadingEnable = VK_TRUE, .minSampleShading = 1};
    VkPipelineColorBlendAttachmentState color = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &color};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &inputs, .pInputAssemblyState = &assembly, .pViewportState = &viewports, .pRasterizationState = &raster,
        .pMultisampleState = &multisample, .pColorBlendState = &blend, .layout = layout, .renderPass = pass};
    VkPipeline result; CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &result));
    vkDestroyShaderModule(device, vs, NULL); vkDestroyShaderModule(device, fs, NULL); return result;
}
int main(int argc, char **argv) {
    assert(argc == 5); // fill.vert.spv, fill.frag.spv, read-0.comp.spv, read-1.comp.spv
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    const char *layer = "VK_LAYER_KHRONOS_validation", *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    VkBool32 validation = vkCreateInstance(&ii, NULL, &instance) == VK_SUCCESS;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (!validation) { ii.enabledLayerCount = ii.enabledExtensionCount = 0; CHECK(vkCreateInstance(&ii, NULL, &instance)); }
    else {
        VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = report};
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        assert(create); CHECK(create(instance, &debug, NULL, &messenger));
    }
    uint32_t count = 8; VkPhysicalDevice devices[8]; CHECK(vkEnumeratePhysicalDevices(instance, &count, devices)); assert(count); gpu = devices[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu, &props);
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(gpu, &features);
    printf("GPU: %s; validation=%u\n", props.deviceName, validation);
    if (!features.sampleRateShading) { fprintf(stderr, "GPU lacks sample shading for test initialization\n"); return 77; }
    VkPhysicalDeviceFeatures enabled = {.sampleRateShading = VK_TRUE};
    VkQueueFamilyProperties families[32]; count = 32; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    unsigned family = 0; VkQueueFlags required = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT;
    while (family < count && (families[family].queueFlags & required) != required) ++family; assert(family < count);
    float priority = 1; VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qi, .pEnabledFeatures = &enabled}; CHECK(vkCreateDevice(gpu, &di, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = {4, 4, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_4_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage image; CHECK(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, image, &requirements);
    VkDeviceMemory image_memory = memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CHECK(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_info.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CHECK(vkCreateImageView(device, &vi, NULL, &view));
    VkAttachmentDescription attachment = {.format = image_info.format, .samples = VK_SAMPLE_COUNT_4_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo frame_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view, .width = 4, .height = 4, .layers = 1};
    VkFramebuffer frame; CHECK(vkCreateFramebuffer(device, &frame_info, NULL, &frame));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16 * 4 * sizeof(float), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer; CHECK(vkCreateBuffer(device, &bi, NULL, &buffer)); vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkDeviceMemory buffer_memory = memory(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0)); float *results; CHECK(vkMapMemory(device, buffer_memory, 0, bi.size, 0, (void **)&results));
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler; CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetLayout layouts[2]; VkPipelineLayout pipeline_layouts[2]; VkDescriptorSet sets[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkDescriptorSetLayoutBinding bindings[] = {{0, i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = i ? 2 : 0, .pBindings = bindings};
        CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &layouts[i]));
        VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layouts[i]};
        CHECK(vkCreatePipelineLayout(device, &pi, NULL, &pipeline_layouts[i]));
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &layouts[i]};
        CHECK(vkAllocateDescriptorSets(device, &ai, &sets[i]));
        VkDescriptorImageInfo im = {i ? sampler : VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buf = {buffer, 0, bi.size};
        VkWriteDescriptorSet writes[] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = bindings[0].descriptorType, .pImageInfo = &im},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = bindings[1].descriptorType, .pBufferInfo = &buf}};
        if (i) vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    }
    VkPipeline pipelines[] = {fill_pipeline(argv[1], argv[2], pipeline_layouts[0], pass), pipeline(argv[3], pipeline_layouts[1]), pipeline(argv[4], pipeline_layouts[1])};
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &ci, NULL, &commands));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}; VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(device, &ca, &cb));
    unsigned checked = 0;
    for (unsigned half = 0; half < 2; ++half) {
        CHECK(vkResetCommandBuffer(cb, 0)); memset(results, 0xff, bi.size);
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &begin));
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = half ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcAccessMask = half ? VK_ACCESS_SHADER_READ_BIT : 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
            .framebuffer = frame, .renderArea = {{0, 0}, {4, 4}}};
        vkCmdBeginRenderPass(cb, &render, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
        vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRenderPass(cb);
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[half + 1]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layouts[1], 0, 1, &sets[1], 0, NULL); vkCmdDispatch(cb, 2, 2, 4);
        VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cb)); VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        for (unsigned sample = 0; sample < 4; ++sample) for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 2; ++x) {
            const float expected[] = {2 * x, 2 * y, sample, 10 + sample};
            for (unsigned channel = 0; channel < 4; ++channel) {
                float got = results[4 * (x + 2 * y + 4 * sample) + channel];
                if (got != expected[channel]) { fprintf(stderr, "half=%u x=%u y=%u sample=%u channel=%u: got %g expected %g\n", half, x, y, sample, channel, got, expected[channel]); return 1; }
                ++checked;
            }
        }
    }
    for (unsigned i = 0; i < 3; ++i) vkDestroyPipeline(device, pipelines[i], NULL);
    vkDestroyCommandPool(device, commands, NULL); vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned i = 0; i < 2; ++i) { vkDestroyPipelineLayout(device, pipeline_layouts[i], NULL); vkDestroyDescriptorSetLayout(device, layouts[i], NULL); }
    vkDestroyFramebuffer(device, frame, NULL); vkDestroyRenderPass(device, pass, NULL);
    vkDestroySampler(device, sampler, NULL); vkUnmapMemory(device, buffer_memory); vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, buffer_memory, NULL); vkDestroyImageView(device, view, NULL); vkDestroyImage(device, image, NULL); vkFreeMemory(device, image_memory, NULL);
    vkDestroyDevice(device, NULL);
    if (messenger) ((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    printf("%u TLDS.MS GPU values passed, %u validation messages\n", checked, messages); return messages != 0;
}
