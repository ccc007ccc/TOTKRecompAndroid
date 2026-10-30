// Immutable per-submission sets: compare resolved values by category, then write
// a fresh set with a Vulkan template. Dynamic offsets belong to the bind command.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __ANDROID__
#include <volk.h>
#else
#include <vulkan/vulkan.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { VK_DESCRIPTOR_UPDATE_MAX = 96, DESCRIPTOR_UNIFORM = 1, DESCRIPTOR_STORAGE = 2,
       DESCRIPTOR_TEXTURE = 4, DESCRIPTOR_IMAGE = 8 };
#define DESCRIPTOR_HIDDEN __attribute__((visibility("hidden")))
typedef union { VkDescriptorBufferInfo buffer; VkDescriptorImageInfo image; } VkDescriptorValue;
typedef struct {
    VkDescriptorUpdateTemplate update;
    uint32_t count, categories;
    uint32_t bindings[VK_DESCRIPTOR_UPDATE_MAX];
    VkDescriptorType types[VK_DESCRIPTOR_UPDATE_MAX];
    VkDescriptorValue values[VK_DESCRIPTOR_UPDATE_MAX];
    uint64_t seq;
    VkDescriptorSet set;
    bool ready;
} VkDescriptorUpdate;
// Render producer owns this state. Creation failure leaves it disabled until destroy/reset.
DESCRIPTOR_HIDDEN VkResult vk_descriptor_update_init(VkDescriptorUpdate *state, VkDevice device, VkDescriptorSetLayout layout,
    uint32_t count, const uint32_t *bindings, const VkDescriptorType *types);
DESCRIPTOR_HIDDEN void vk_descriptor_update_destroy(VkDescriptorUpdate *state, VkDevice device);
// No hashing or Vulkan calls. The caller also checks set/seq, including empty layouts (no category bits).
DESCRIPTOR_HIDDEN uint32_t vk_descriptor_update_dirty(const VkDescriptorUpdate *state, uint64_t seq, const VkDescriptorValue *values);
// Never writes descriptors into the previous set: caller supplies a fresh set for any dirty category/new sequence.
DESCRIPTOR_HIDDEN void vk_descriptor_update_write(VkDescriptorUpdate *state, VkDevice device, uint64_t seq, VkDescriptorSet fresh,
    const VkDescriptorValue *values);
#ifdef __cplusplus
}
#endif
