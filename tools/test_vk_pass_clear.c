// Real Vulkan equivalence of the production loadOp helper and attachment
// clears, including untouched mips/layers/aspects, classic passes and worker.
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/nvn/vulkan/command_worker.h"
#include "../runtime/nvn/vulkan/pass_clear.h"
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
// The desktop fixture owns and destroys the classic-pass cache at shutdown.
#include "../runtime/nvn/vulkan/rpcompat.c"
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); abort(); } } while (0)
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); abort(); }
void vk_device_fault_report(void) { abort(); }
uint64_t vk_pending_seq(void) { return 1; }
static _Atomic unsigned messages;
static VkDebugUtilsMessengerEXT messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)severity; (void)type; (void)user; atomic_fetch_add(&messages, 1);
    fprintf(stderr, "validation: %s\n", data->pMessage); return VK_FALSE;
}
static VkDeviceMemory allocate(VkMemoryRequirements r, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &p);
    for (unsigned i = 0; i < p.memoryTypeCount; ++i)
        if ((r.memoryTypeBits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) {
            VkMemoryAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = i};
            VkDeviceMemory m; CHECK(vkAllocateMemory(g_vk.dev, &a, NULL, &m)); return m;
        }
    abort();
}
static void initialize(void) {
    CHECK(volkInitialize());
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const char *layer = "VK_LAYER_KHRONOS_validation", *ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkValidationFeatureEnableEXT enable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enable};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &vf, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext};
    CHECK(vkCreateInstance(&ii, NULL, &g_vk.inst)); volkLoadInstance(g_vk.inst);
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = report};
    CHECK(vkCreateDebugUtilsMessengerEXT(g_vk.inst, &debug, NULL, &messenger));
    uint32_t n = 16; VkPhysicalDevice devices[16]; CHECK(vkEnumeratePhysicalDevices(g_vk.inst, &n, devices)); assert(n);
    g_vk.phys = devices[0]; vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    printf("GPU: %s; synchronization validation required\n", g_vk.props.deviceName);
    assert(g_vk.props.apiVersion >= VK_API_VERSION_1_3);
    VkQueueFamilyProperties families[32]; n = 32; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &n, families);
    while (g_vk.qfam < n && !(families[g_vk.qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++g_vk.qfam;
    assert(g_vk.qfam < n); float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &features); assert(have.dynamicRendering && have.synchronization2);
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev)); volkLoadDevice(g_vk.dev);
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
    CHECK(vkCreateCommandPool(g_vk.dev, &pool, NULL, &g_vk.pool));
}
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView views[2][3]; } Image;
static Image image(VkFormat format, VkImageAspectFlags aspects) {
    Image result = {0};
    VkImageCreateInfo i = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {32, 16, 1}, .mipLevels = 2, .arrayLayers = 3, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (aspects == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)};
    CHECK(vkCreateImage(g_vk.dev, &i, NULL, &result.image)); VkMemoryRequirements r;
    vkGetImageMemoryRequirements(g_vk.dev, result.image, &r); result.memory = allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindImageMemory(g_vk.dev, result.image, result.memory, 0));
    for (unsigned mip = 0; mip < 2; ++mip)
    for (unsigned layer = 0; layer < 3; ++layer) {
        VkImageViewCreateInfo v = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = result.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = {aspects, mip, 1, layer, 1}};
        CHECK(vkCreateImageView(g_vk.dev, &v, NULL, &result.views[mip][layer]));
    }
    return result;
}
static void destroy_image(Image *im) {
    for (unsigned mip = 0; mip < 2; ++mip) for (unsigned layer = 0; layer < 3; ++layer) {
        // Classic framebuffer ownership uses the production retirement hook.
        bool dynamic = g_vk.dyn_render; g_vk.dyn_render = false;
        vk_rp_forget_view(im->views[mip][layer]); g_vk.dyn_render = dynamic;
        vkDestroyImageView(g_vk.dev, im->views[mip][layer], NULL);
    }
    vkDestroyImage(g_vk.dev, im->image, NULL); vkFreeMemory(g_vk.dev, im->memory, NULL);
}
static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
enum { PIXELS = (32 * 16 + 16 * 8) * 3, BYTES = PIXELS * 9 };
static void copy_all(VkCommandBuffer cb, Image *im, VkImageAspectFlags aspect, VkBuffer out, unsigned offset, unsigned stride) {
    VkBufferImageCopy regions[6]; unsigned k = 0;
    for (unsigned mip = 0; mip < 2; ++mip) for (unsigned layer = 0; layer < 3; ++layer) {
        unsigned w = 32 >> mip, h = 16 >> mip;
        regions[k++] = (VkBufferImageCopy){.bufferOffset = offset, .imageSubresource = {aspect, mip, layer, 1}, .imageExtent = {w, h, 1}};
        offset += w * h * stride;
    }
    vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, out, 6, regions);
}
static void record(VkCommandBuffer cb, Image *color, Image *ds, VkFormat format, VkBuffer out,
                   unsigned scenario, bool optimize, bool first) {
    VkImageMemoryBarrier2 init[2];
    for (unsigned i = 0; i < 2; ++i) init[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = first ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = i ? ds->image : color->image, .subresourceRange = {i ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 3}};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = init};
    vkCmdPipelineBarrier2(cb, &dep);
    VkClearColorValue sentinel = {.float32 = {1, 0, 1, 1}};
    if (format == VK_FORMAT_R32_UINT) sentinel.uint32[0] = 0x10203040;
    if (format == VK_FORMAT_R32_SINT) sentinel.int32[0] = -42;
    VkClearDepthStencilValue old_ds = {0.75f, 0xa5};
    vkCmdClearColorImage(cb, color->image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1, &init[0].subresourceRange);
    vkCmdClearDepthStencilImage(cb, ds->image, VK_IMAGE_LAYOUT_GENERAL, &old_ds, 1, &init[1].subresourceRange);
    barrier(cb);
    unsigned mip = scenario == 1 || scenario == 9, layer = mip ? 2 : 0;
    VkExtent2D target = {32 >> mip, 16 >> mip}, area = target;
    VkRect2D rect = {{0, 0}, area};
    bool is_color = scenario == 0 || scenario == 1 || scenario == 6 || scenario == 8;
    if (scenario == 5 || scenario == 6) { rect.offset.x = 3; rect.extent.width -= 5; }
    if (scenario == 7) { area.width /= 2; rect.extent = area; }
    VkPassClear request = {.index = is_color ? 0 : -1, .color_mask = 15, .stencil_mask = 255,
        .depth = !is_color && scenario != 3, .stencil = !is_color && scenario != 2 && scenario != 5};
    if (is_color) {
        request.value.color = (VkClearColorValue){.float32 = {1, 1, 0, 1}};
        if (format == VK_FORMAT_R32_UINT) request.value.color.uint32[0] = 0x89abcdef;
        if (format == VK_FORMAT_R32_SINT) request.value.color.int32[0] = -1234567;
        if (scenario == 8) memset(&request.value.color, 0, sizeof request.value.color);
    } else request.value.depthStencil = (VkClearDepthStencilValue){0.25f, 0x37};
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = color->views[mip][layer],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo da = ca, sa = ca; da.imageView = sa.imageView = ds->views[mip][layer];
    bool folded = optimize && vk_pass_clear_apply(&request, rect, area, target, &ca, 1, &da, &sa);
    assert(folded == (optimize && scenario != 5 && scenario != 6 && scenario != 7));
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, area}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da, .pStencilAttachment = &sa};
    vk_begin_rendering(cb, &ri, &format, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_SAMPLE_COUNT_1_BIT);
    if (!folded) {
        VkClearAttachment clear = {.aspectMask = is_color ? VK_IMAGE_ASPECT_COLOR_BIT :
            (request.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (request.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
            .colorAttachment = 0, .clearValue = request.value};
        VkClearRect r = {rect, 0, 1}; vkCmdClearAttachments(cb, 1, &clear, 1, &r);
    }
    vk_end_rendering(cb);
    // Poison producer-local payloads before flushing the command worker.
    memset(&ca, 0xcd, sizeof ca); memset(&da, 0xcd, sizeof da); memset(&sa, 0xcd, sizeof sa); memset(&ri, 0xcd, sizeof ri);
    barrier(cb);
    copy_all(cb, color, VK_IMAGE_ASPECT_COLOR_BIT, out, 0, 4);
    copy_all(cb, ds, VK_IMAGE_ASPECT_DEPTH_BIT, out, PIXELS * 4, 4);
    copy_all(cb, ds, VK_IMAGE_ASPECT_STENCIL_BIT, out, PIXELS * 8, 1);
    VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    dep = (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &host};
    vkCmdPipelineBarrier2(cb, &dep);
}
static void expected(const uint8_t *data, VkFormat format, unsigned scenario) {
    unsigned offset = 0, selected_mip = scenario == 1 || scenario == 9, selected_layer = selected_mip ? 2 : 0;
    bool color = scenario == 0 || scenario == 1 || scenario == 6 || scenario == 8;
    for (unsigned mip = 0; mip < 2; ++mip) for (unsigned layer = 0; layer < 3; ++layer)
    for (unsigned y = 0; y < (16u >> mip); ++y) for (unsigned x = 0; x < (32u >> mip); ++x, ++offset) {
        bool changed = mip == selected_mip && layer == selected_layer;
        if (scenario == 5 || scenario == 6) changed &= x >= 3 && x < (32u >> mip) - 2;
        if (scenario == 7) changed &= x < (32u >> mip) / 2;
        uint32_t c; float d; memcpy(&c, data + offset * 4, 4); memcpy(&d, data + PIXELS * 4 + offset * 4, 4);
        uint8_t s = data[PIXELS * 8 + offset];
        uint32_t old_color = format == VK_FORMAT_R8G8B8A8_UNORM ? 0xffff00ff : format == VK_FORMAT_R32_UINT ? 0x10203040 : (uint32_t)-42;
        uint32_t new_color = scenario == 8 ? 0 : format == VK_FORMAT_R8G8B8A8_UNORM ? 0xff00ffff : format == VK_FORMAT_R32_UINT ? 0x89abcdef : (uint32_t)-1234567;
        assert(c == (changed && color ? new_color : old_color));
        assert(d == (changed && !color && scenario != 3 ? 0.25f : 0.75f));
        assert(s == (changed && !color && scenario != 2 && scenario != 5 ? 0x37 : 0xa5));
    }
    assert(offset == PIXELS);
}
int main(void) {
    initialize();
    VkBuffer out; VkDeviceMemory memory;
    VkBufferCreateInfo b = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(g_vk.dev, &b, NULL, &out)); VkMemoryRequirements r; vkGetBufferMemoryRequirements(g_vk.dev, out, &r);
    memory = allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(g_vk.dev, out, memory, 0)); uint8_t *mapped; CHECK(vkMapMemory(g_vk.dev, memory, 0, BYTES, 0, (void**)&mapped));
    uint8_t baseline[BYTES]; unsigned comparisons = 0;
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    for (unsigned dynamic = 0; dynamic < 2; ++dynamic) for (unsigned worker = 0; worker < 2; ++worker) {
        g_vk.dyn_render = dynamic;
        if (worker) assert(vk_command_worker_start(true));
        for (unsigned f = 0; f < 3; ++f) {
            Image color = image(formats[f], VK_IMAGE_ASPECT_COLOR_BIT);
            Image ds = image(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            bool first = true;
            for (unsigned scenario = 0; scenario < 10; ++scenario) for (unsigned optimize = 0; optimize < 2; ++optimize) {
                CHECK(vkResetCommandPool(g_vk.dev, g_vk.pool, 0));
                VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool = g_vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
                CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &cb));
                VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                CHECK(vkBeginCommandBuffer(cb, &bi)); record(cb, &color, &ds, formats[f], out, scenario, optimize, first); first = false;
                vk_command_worker_flush(); CHECK(vkEndCommandBuffer(cb));
                VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
                CHECK(vkQueueSubmit(g_vk.queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(g_vk.queue));
                expected(mapped, formats[f], scenario);
                if (!optimize) memcpy(baseline, mapped, BYTES);
                else { assert(!memcmp(baseline, mapped, BYTES)); ++comparisons; }
                vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
            }
            destroy_image(&color); destroy_image(&ds);
        }
        if (worker) { VkCommandWorkerStats stats = vk_command_worker_stats(); assert(stats.commands && !stats.fallback); vk_command_worker_stop(); }
    }
    vkUnmapMemory(g_vk.dev, memory); vkDestroyBuffer(g_vk.dev, out, NULL); vkFreeMemory(g_vk.dev, memory, NULL);
    vkDestroyCommandPool(g_vk.dev, g_vk.pool, NULL);
    for (int i = 0; i < nrps; ++i) vkDestroyRenderPass(g_vk.dev, rps[i].rp, NULL);
    free(late);
    vkDestroyDevice(g_vk.dev, NULL);
    vkDestroyDebugUtilsMessengerEXT(g_vk.inst, messenger, NULL); vkDestroyInstance(g_vk.inst, NULL);
    assert(!atomic_load(&messages));
    printf("PASS: %u GPU comparisons; float/uint/sint, zero, depth/stencil separation, partial/scaled area, mips/layers, classic/dynamic and worker\n", comparisons);
}
