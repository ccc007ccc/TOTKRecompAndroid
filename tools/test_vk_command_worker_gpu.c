// Real Vulkan / synchronization validation: command recording on/off must deliver
// the same independent compute + image-clear values through submit, reuse and stop.
#include "../runtime/nvn/vulkan/command_worker.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { VkResult r = (expr); if (r != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #expr, r); abort(); } } while (0)
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkDescriptorPool descriptors;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkPipeline pipe;
static VkDebugUtilsMessengerEXT messenger;
static _Atomic unsigned validation_messages;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint8_t *mapped; } Buffer;
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; } Image;
static Buffer output, input;
static Image color, depth;
static uint64_t uniform_stride;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)severity; (void)type; (void)user; atomic_fetch_add(&validation_messages, 1);
    fprintf(stderr, "validation: %s\n", data->pMessage); return VK_FALSE;
}
static VkDeviceMemory allocate(VkMemoryRequirements r, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    unsigned i = 0; for (; i < p.memoryTypeCount; i++)
        if ((r.memoryTypeBits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) break;
    assert(i < p.memoryTypeCount);
    VkMemoryAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = i};
    VkDeviceMemory m; CHECK(vkAllocateMemory(device, &a, NULL, &m)); return m;
}
static Buffer buffer(uint64_t size, VkBufferUsageFlags usage) {
    Buffer b = {0}; VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(device, &ci, NULL, &b.buffer)); VkMemoryRequirements r; vkGetBufferMemoryRequirements(device, b.buffer, &r);
    b.memory = allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0)); CHECK(vkMapMemory(device, b.memory, 0, size, 0, (void **)&b.mapped)); return b;
}
static Image image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image im = {0}; VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {32, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    CHECK(vkCreateImage(device, &ci, NULL, &im.image)); VkMemoryRequirements r; vkGetImageMemoryRequirements(device, im.image, &r);
    im.memory = allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CHECK(vkBindImageMemory(device, im.image, im.memory, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .subresourceRange = {aspect, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(device, &vi, NULL, &im.view)); return im;
}
static void initialize(const char *path) {
    CHECK(volkInitialize());
    const char *layer = "VK_LAYER_KHRONOS_validation", *ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkValidationFeatureEnableEXT enable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enable};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext, .pNext = &vf};
    CHECK(vkCreateInstance(&ci, NULL, &instance)); volkLoadInstance(instance);
    VkDebugUtilsMessengerCreateInfoEXT di = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = report};
    CHECK(vkCreateDebugUtilsMessengerEXT(instance, &di, NULL, &messenger));
    uint32_t n = 16; VkPhysicalDevice devices[16]; CHECK(vkEnumeratePhysicalDevices(instance, &n, devices)); assert(n);
    gpu = devices[0]; VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu, &props);
    assert(props.apiVersion >= VK_API_VERSION_1_3); uniform_stride = props.limits.minUniformBufferOffsetAlignment;
    if (uniform_stride < 16) uniform_stride = 16;
    printf("GPU: %s (synchronization validation enabled)\n", props.deviceName);
    uint32_t nf = 32; VkQueueFamilyProperties families[32]; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, families);
    uint32_t family = 0; while (family < nf && (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) family++;
    assert(family < nf); float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(gpu, &dc, NULL, &device)); volkLoadDevice(device); vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family}; CHECK(vkCreateCommandPool(device, &pi, NULL, &pool));
    output = buffer(16384, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    input = buffer(8 * uniform_stride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    color = image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    depth = image(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &set_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    CHECK(vkCreatePipelineLayout(device, &pl, NULL, &layout));
    FILE *f = fopen(path, "rb"); assert(f); assert(!fseek(f, 0, SEEK_END)); long bytes = ftell(f); assert(bytes > 0 && !(bytes & 3)); rewind(f);
    uint32_t *code = malloc((size_t)bytes); assert(code); assert(fread(code, 1, bytes, f) == (size_t)bytes); fclose(f);
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule shader; CHECK(vkCreateShaderModule(device, &sm, NULL, &shader)); free(code);
    VkComputePipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}, .layout = layout};
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, NULL, &pipe)); vkDestroyShaderModule(device, shader, NULL);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    CHECK(vkCreateDescriptorPool(device, &dp, NULL, &descriptors));
}
static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa, .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m}; vkCmdPipelineBarrier2(cb, &d);
}
static void record(VkCommandBuffer cb, unsigned repetition) {
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorBufferInfo infos[] = {{output.buffer, 0, 4096}, {input.buffer, 0, 16}};
    VkWriteDescriptorSet writes[2]; for (unsigned i = 0; i < 2; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = i, .descriptorCount = 1, .descriptorType = i ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &infos[i]};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    for (unsigned i = 0; i < 8; i++) {
        uint32_t offset = i * uniform_stride, pc[] = {i * 128, repetition * 100};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1, &offset);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc); vkCmdDispatch(cb, 4, 1, 1);
        offset = 0; memset(pc, 0, sizeof pc);
        memory_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    // Repeated state packets force several published batches without changing output.
    VkViewport vp = {0, 0, 32, 16, 0, 1}; for (unsigned i = 0; i < 300; i++) vkCmdSetViewport(cb, 0, 1, &vp);
    VkImageMemoryBarrier2 barriers[2] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = color.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = depth.image, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}}};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers}; vkCmdPipelineBarrier2(cb, &dep);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = color.view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = {1, 0, 1, 1}};
    VkRenderingAttachmentInfo ds = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.depthStencil = {.25f, 0x77}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {32, 16}, .layerCount = 1, .colorAttachmentCount = 1,
        .pColorAttachments = &ca, .pDepthAttachment = &ds, .pStencilAttachment = &ds};
    vkCmdBeginRendering(cb, &ri); vkCmdEndRendering(cb); memset(&ca, 0, sizeof ca); memset(&ds, 0, sizeof ds);
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL; barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    dep.imageMemoryBarrierCount = 1; vkCmdPipelineBarrier2(cb, &dep);
    VkBufferImageCopy copy = {.bufferOffset = 8192, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {32, 16, 1}};
    vkCmdCopyImageToBuffer(cb, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy); memset(&copy, 0, sizeof copy);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
static void run(bool worker_on) {
    if (worker_on) assert(vk_command_worker_start(false));
    VkCommandBuffer buffers[2]; VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2}; CHECK(vkAllocateCommandBuffers(device, &ai, buffers));
    for (unsigned repetition = 0; repetition < 3; repetition++) {
        CHECK(vkResetCommandPool(device, pool, 0)); CHECK(vkResetDescriptorPool(device, descriptors, 0));
        memset(output.mapped, 0xcd, 16384); for (unsigned i = 0; i < 8; i++) *(uint32_t *)(input.mapped + i * uniform_stride) = 1000 + 10 * i;
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(buffers[0], &bi)); record(buffers[0], repetition);
        // Same pool's other buffer must not begin until outstanding recordings retire.
        CHECK(vkBeginCommandBuffer(buffers[1], &bi)); CHECK(vkEndCommandBuffer(buffers[1]));
        if (worker_on && repetition == 2) vk_command_worker_stop(); // publish partial final batch before end
        CHECK(vkEndCommandBuffer(buffers[0]));
        if (repetition & 1) {
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 2, .pCommandBuffers = buffers}; CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        } else {
            VkCommandBufferSubmitInfo cis[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = buffers[0]},
                {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = buffers[1]}};
            VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 2, .pCommandBufferInfos = cis}; CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
        }
        CHECK(vkQueueWaitIdle(queue));
        for (unsigned i = 0; i < 1024; i++) assert(((uint32_t *)output.mapped)[i] == 1000 + 10 * (i / 128) + repetition * 100 + i % 128);
        for (unsigned i = 0; i < 512; i++) assert(((uint32_t *)(output.mapped + 8192))[i] == 0xffff00ffu);
        assert(!atomic_load(&validation_messages));
    }
    vkFreeCommandBuffers(device, pool, 2, buffers);
    if (worker_on) assert(vk_command_worker_stats().commands > 900);
}
static void cleanup_image(Image im) { vkDestroyImageView(device, im.view, NULL); vkDestroyImage(device, im.image, NULL); vkFreeMemory(device, im.memory, NULL); }
static void cleanup_buffer(Buffer b) { vkUnmapMemory(device, b.memory); vkDestroyBuffer(device, b.buffer, NULL); vkFreeMemory(device, b.memory, NULL); }
int main(int argc, char **argv) {
    assert(argc == 2); initialize(argv[1]); run(false); run(true);
    CHECK(vkDeviceWaitIdle(device)); vkDestroyDescriptorPool(device, descriptors, NULL); vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipe, NULL); vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    cleanup_image(color); cleanup_image(depth); cleanup_buffer(output); cleanup_buffer(input);
    vkDestroyDevice(device, NULL); assert(!atomic_load(&validation_messages));
    vkDestroyDebugUtilsMessengerEXT(instance, messenger, NULL); vkDestroyInstance(instance, NULL);
    puts("PASS: worker on/off compute and image values, dynamic offsets, shared command pool handoff, submit1/2, reuse and stop; zero validation messages");
}
