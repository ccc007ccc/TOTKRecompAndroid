#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

// Compose the bundled capture layer with the already-selected driver. This
// does not modify Android's global GPU-debug settings or choose another ICD.
PFN_vkGetInstanceProcAddr rd_layer_load(PFN_vkGetInstanceProcAddr driver, const char *library,
                                      char *error, size_t error_size);
void *rd_layer_api(void);
int rd_layer_capture_fd(int fd);
bool rd_capture_available(void);
int rd_capture_begin(VkInstance instance, char *error, size_t error_size);
bool rd_capture_end(VkInstance instance);
void rd_capture_discard(VkInstance instance);
#ifdef __cplusplus
}
#endif
