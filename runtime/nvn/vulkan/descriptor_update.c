#include "descriptor_update.h"
#include <string.h>

static uint32_t category(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return DESCRIPTOR_UNIFORM;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return DESCRIPTOR_STORAGE;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return DESCRIPTOR_TEXTURE;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return DESCRIPTOR_IMAGE;
    default: return 0;
    }
}
VkResult vk_descriptor_update_init(VkDescriptorUpdate *s, VkDevice device, VkDescriptorSetLayout layout,
    uint32_t count, const uint32_t *bindings, const VkDescriptorType *types) {
    // Initialization is one-shot; destroy owns an already created template.
    if (!s || s->ready || s->update || count > VK_DESCRIPTOR_UPDATE_MAX || (count && (!bindings || !types)))
        return VK_ERROR_INITIALIZATION_FAILED;
    memset(s, 0, sizeof *s);
#ifdef __ANDROID__
    if (!vkCreateDescriptorUpdateTemplate || !vkDestroyDescriptorUpdateTemplate || !vkUpdateDescriptorSetWithTemplate)
        return VK_ERROR_FEATURE_NOT_PRESENT;
#endif
    VkDescriptorUpdateTemplateEntry entries[VK_DESCRIPTOR_UPDATE_MAX];
    uint32_t categories = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = category(types[i]);
        if (!c) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (uint32_t j = 0; j < i; j++) if (bindings[i] == bindings[j]) return VK_ERROR_INITIALIZATION_FAILED;
        categories |= c;
        entries[i] = (VkDescriptorUpdateTemplateEntry){.dstBinding = bindings[i], .descriptorCount = 1,
            .descriptorType = types[i], .offset = i * sizeof(VkDescriptorValue), .stride = sizeof(VkDescriptorValue)};
    }
    VkDescriptorUpdateTemplate update = VK_NULL_HANDLE;
    if (count) {
        VkDescriptorUpdateTemplateCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .descriptorUpdateEntryCount = count, .pDescriptorUpdateEntries = entries,
            .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = layout};
        VkResult result = vkCreateDescriptorUpdateTemplate(device, &ci, NULL, &update);
        if (result != VK_SUCCESS) return result;
    }
    s->update = update; s->count = count; s->categories = categories; s->ready = true;
    if (count) { memcpy(s->bindings, bindings, count * sizeof *bindings); memcpy(s->types, types, count * sizeof *types); }
    return VK_SUCCESS;
}
void vk_descriptor_update_destroy(VkDescriptorUpdate *s, VkDevice device) {
    if (!s) return;
    if (s->update) vkDestroyDescriptorUpdateTemplate(device, s->update, NULL);
    memset(s, 0, sizeof *s);
}
static bool same_buffer(VkDescriptorBufferInfo a, VkDescriptorBufferInfo b) {
    return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
}
static bool same_image(VkDescriptorImageInfo a, VkDescriptorImageInfo b) {
    return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
}
uint32_t vk_descriptor_update_dirty(const VkDescriptorUpdate *s, uint64_t seq, const VkDescriptorValue *values) {
    if (!s->set || s->seq != seq) return s->categories;
    uint32_t dirty = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        uint32_t c = category(s->types[i]);
        if (dirty & c) continue;
        bool same = (c & (DESCRIPTOR_TEXTURE | DESCRIPTOR_IMAGE)) ? same_image(values[i].image, s->values[i].image)
                                                                : same_buffer(values[i].buffer, s->values[i].buffer);
        if (!same) dirty |= c;
    }
    return dirty;
}
void vk_descriptor_update_write(VkDescriptorUpdate *s, VkDevice device, uint64_t seq, VkDescriptorSet fresh,
    const VkDescriptorValue *values) {
    if (s->count) vkUpdateDescriptorSetWithTemplate(device, fresh, s->update, values);
    if (s->count) memcpy(s->values, values, s->count * sizeof *values);
    s->seq = seq; s->set = fresh;
}
