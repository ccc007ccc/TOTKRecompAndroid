#pragma once
#include <stdbool.h>
#include <string.h>
#include <vulkan/vulkan.h>

// One synchronous clear at a pass boundary. No texture or view survives the
// call: aliases, copies, readbacks and submissions still use their existing order.
typedef struct {
    int index;
    uint32_t color_mask, stencil_mask;
    bool depth, stencil;
    VkClearValue value;
} VkPassClear;

static inline bool vk_pass_clear_apply(const VkPassClear *clear, VkRect2D rect, VkExtent2D area,
                                      VkExtent2D target, VkRenderingAttachmentInfo *color,
                                      uint32_t ncolor, VkRenderingAttachmentInfo *depth,
                                      VkRenderingAttachmentInfo *stencil) {
    if (!clear || !area.width || !area.height || rect.offset.x || rect.offset.y ||
        rect.extent.width != area.width || rect.extent.height != area.height ||
        target.width != area.width || target.height != area.height)
        return false;
    if (clear->index >= 0) {
        if ((uint32_t)clear->index >= ncolor || !color[clear->index].imageView ||
            (clear->color_mask & 15) != 15 || clear->depth || clear->stencil)
            return false;
        color[clear->index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color[clear->index].clearValue = clear->value;
        return true;
    }
    // Require every requested aspect to exist; a depth-only clear must never
    // replace stencil's LOAD (or vice versa). Partial stencil stays a draw.
    if ((!clear->depth && !clear->stencil) ||
        (clear->depth && (!depth || !depth->imageView)) ||
        (clear->stencil && (!stencil || !stencil->imageView || (clear->stencil_mask & 255) != 255)))
        return false;
    if (clear->depth) { depth->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth->clearValue = clear->value; }
    if (clear->stencil) { stencil->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; stencil->clearValue = clear->value; }
    return true;
}
