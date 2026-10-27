#define _GNU_SOURCE
#include "renderdoc_bridge.h"
#include <stdbool.h>
#include "renderdoc_app.h"
#include <vulkan/vk_layer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr driver_gipa, capture_gipa;
static PFN_vkGetDeviceProcAddr driver_gdpa;
static VkInstance driver_instance, capture_instance;
static RENDERDOC_API_1_6_0 *api;
static int (*set_capture_fd)(int);

// The system loader beneath this chain constructs its own loader records.
// Strip only our leading loader records, preserving application feature chains.
static const void *without_loader_records(const void *p) {
    const VkBaseInStructure *s = p;
    while (s && (s->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                 s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)) s = s->pNext;
    return s;
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_create_instance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkInstance *instance) {
    VkInstanceCreateInfo ci = *info;
    ci.pNext = without_loader_records(ci.pNext);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)driver_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(&ci, alloc, instance);
    if (r == VK_SUCCESS) {
        driver_instance = *instance;
        driver_gdpa = (PFN_vkGetDeviceProcAddr)driver_gipa(*instance, "vkGetDeviceProcAddr");
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_create_device(VkPhysicalDevice physical,
        const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc, VkDevice *device) {
    VkDeviceCreateInfo ci = *info;
    ci.pNext = without_loader_records(ci.pNext);
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)driver_gipa(driver_instance, "vkCreateDevice");
    return create(physical, &ci, alloc, device);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance instance, const char *name) {
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)driver_create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)driver_create_device;
    return driver_gipa(instance, name);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL next_gdpa(VkDevice device, const char *name) {
    return driver_gdpa(device, name);
}
static VKAPI_ATTR VkResult VKAPI_CALL capture_create_instance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkInstance *instance) {
    VkLayerInstanceLink link = {.pfnNextGetInstanceProcAddr = next_gipa};
    VkLayerInstanceCreateInfo chain = {.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
        .pNext = info->pNext, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link};
    VkInstanceCreateInfo ci = *info; ci.pNext = &chain;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)capture_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(&ci, alloc, instance);
    if (r == VK_SUCCESS) capture_instance = *instance;
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL capture_create_device(VkPhysicalDevice physical,
        const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc, VkDevice *device) {
    VkLayerDeviceLink link = {.pfnNextGetInstanceProcAddr = next_gipa, .pfnNextGetDeviceProcAddr = next_gdpa};
    VkLayerDeviceCreateInfo chain = {.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        .pNext = info->pNext, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link};
    VkDeviceCreateInfo ci = *info; ci.pNext = &chain;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)capture_gipa(capture_instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    return create(physical, &ci, alloc, device);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL composed_gipa(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)composed_gipa;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)capture_create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)capture_create_device;
    if (!instance) return driver_gipa(instance, name);
    return capture_gipa(instance, name);
}
PFN_vkGetInstanceProcAddr rd_layer_load(PFN_vkGetInstanceProcAddr driver, const char *library,
                                      char *error, size_t error_size) {
    if (api) return composed_gipa;
    void *lib = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { snprintf(error, error_size, "%s", dlerror()); return NULL; }
    capture_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "VK_LAYER_RENDERDOC_CaptureGetInstanceProcAddr");
    pRENDERDOC_GetAPI get = (pRENDERDOC_GetAPI)dlsym(lib, "RENDERDOC_GetAPI");
    set_capture_fd = (int (*)(int))dlsym(lib, "RENDERDOC_SetCaptureFileFD");
    if (!capture_gipa || !get || !set_capture_fd || !get(eRENDERDOC_API_Version_1_6_0, (void **)&api)) {
        snprintf(error, error_size, "Capture layer lacks the required memory capture API");
        api = NULL; return NULL;
    }
    driver_gipa = driver;
    api->MaskOverlayBits(eRENDERDOC_Overlay_None, 0);
    api->SetCaptureKeys(NULL, 0);
    api->SetFocusToggleKeys(NULL, 0);
    api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 0);
    api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 0);
    return composed_gipa;
}
void *rd_layer_api(void) { return api; }
int rd_layer_capture_fd(int fd) { return set_capture_fd ? set_capture_fd(fd) : 0; }
bool rd_capture_available(void) { return api != NULL; }
int rd_capture_begin(VkInstance instance, char *error, size_t error_size) {
    if (!api) { snprintf(error, error_size, "GPU capture library is unavailable"); return -1; }
    int fd = memfd_create("totk-gpu-capture", MFD_CLOEXEC);
    if (fd < 0) { snprintf(error, error_size, "Capture memory: %s", strerror(errno)); return -1; }
    if (!rd_layer_capture_fd(fd)) {
        close(fd); snprintf(error, error_size, "Could not select capture memory"); return -1;
    }
    api->StartFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), NULL);
    if (!api->IsFrameCapturing()) {
        rd_layer_capture_fd(-1); close(fd);
        snprintf(error, error_size, "Could not start GPU capture"); return -1;
    }
    return fd;
}
bool rd_capture_end(VkInstance instance) {
    bool ok = api && api->EndFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), NULL);
    rd_layer_capture_fd(-1);
    return ok;
}
void rd_capture_discard(VkInstance instance) {
    if (api && api->IsFrameCapturing())
        api->DiscardFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), NULL);
    rd_layer_capture_fd(-1);
}
