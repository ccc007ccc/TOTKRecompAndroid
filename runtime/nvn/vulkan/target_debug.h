#include "../../diagnostics_build.h"
// Opt-in attachment snapshots at the end of passes into one guest texture.
// Unlike sampled depth inputs, these include the depth image used by hardware
// depth testing. Readback waits change timing; no work is added unless requested.
static struct {
    TexBackend *color, *depth;
    uint64_t frame, color_addr, depth_addr;
    uint32_t number, color_level, depth_level, color_layer, depth_layer;
    VkAttachmentLoadOp color_load;
} target_debug;

static void debug_target_begin(const VkRenderingAttachmentInfo *colors) {
    static uint64_t wanted;
    static bool initialized;
    static uint64_t frame = ~0ull;
    static uint32_t number;
    target_debug.color = target_debug.depth = NULL;
    if (!initialized) {
        const char *s = diag_getenv("LARECOMP_DUMP_TARGET");
        wanted = s ? strtoull(s, NULL, 16) : 0;
        initialized = true;
    }
    if (!wanted || !g_debug_frame) return;
    uint64_t now = renderer_present_count();
    if (now != frame) { frame = now; number = 0; }
    if (number >= 32) return;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t || t->addr != wanted || !colors[i].imageView) continue;
        target_debug.color = t->backend;
        target_debug.depth = st.rt.depth ? st.rt.depth->backend : NULL;
        target_debug.frame = now;
        target_debug.number = number++;
        target_debug.color_addr = t->addr;
        target_debug.depth_addr = st.rt.depth ? st.rt.depth->addr : 0;
        target_debug.color_load = colors[i].loadOp;
        const NvnTextureView *v = &st.rt.cview[i], *d = &st.rt.dview;
        target_debug.color_level = st.rt.has_cview[i] && (v->setmask & 1) ? v->base_level : 0;
        target_debug.color_layer = st.rt.has_cview[i] && (v->setmask & 2) ? v->min_layer : 0;
        target_debug.depth_level = st.rt.has_dview && (d->setmask & 1) ? d->base_level : 0;
        target_debug.depth_layer = st.rt.has_dview && (d->setmask & 2) ? d->min_layer : 0;
        return;
    }
}

static void debug_target_end(void) {
    if (!target_debug.color) return;
    TexBackend *images[3] = {target_debug.color, target_debug.depth, target_debug.depth};
    // Clear before submitting: renderer_flush itself ends the current pass.
    target_debug.color = target_debug.depth = NULL;
    VkImageAspectFlags aspects[3] = {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT};
    const char *names[3] = {"color", "depth", "stencil"};
    uint64_t offsets[3] = {0}, sizes[3] = {0}, total = 0;
    VkBufferImageCopy copies[3] = {0};
    for (int i = 0; i < 3; i++) {
        TexBackend *b = images[i];
        if (!b || !(b->aspect & aspects[i]) || b->samples != VK_SAMPLE_COUNT_1_BIT || b->depth != 1) continue;
        uint32_t bytes = 0;
        if (!i) {
            if (b->format == VK_FORMAT_R16G16B16A16_SFLOAT) bytes = 8;
            if (b->format == VK_FORMAT_R8G8B8A8_UNORM || b->format == VK_FORMAT_R32_SFLOAT) bytes = 4;
        } else if (i == 2) bytes = 1;
        else if (b->format == VK_FORMAT_D32_SFLOAT || b->format == VK_FORMAT_D32_SFLOAT_S8_UINT) bytes = 4;
        else if (b->format == VK_FORMAT_D16_UNORM) bytes = 2;
        if (!bytes) continue;
        uint32_t level = i ? target_debug.depth_level : target_debug.color_level;
        uint32_t layer = i ? target_debug.depth_layer : target_debug.color_layer;
        if (level >= b->levels || layer >= b->layers) continue;
        uint32_t w = b->width >> level, h = b->height >> level;
        if (!w) w = 1;
        if (!h) h = 1;
        offsets[i] = total;
        sizes[i] = (uint64_t)w * h * bytes;
        total += (sizes[i] + 255) & ~255ull;
        copies[i] = (VkBufferImageCopy){.bufferOffset = offsets[i],
            .imageSubresource = {aspects[i], level, layer, 1}, .imageExtent = {w, h, 1}};
    }
    if (!total || total > (64ull << 20)) return;
    mkdir("build/target-input", 0755);
    char base[128], path[160];
    snprintf(base, sizeof base, "build/target-input/%llu-%02u", (unsigned long long)target_debug.frame, target_debug.number);
    snprintf(path, sizeof path, "%s.txt", base);
    FILE *meta = fopen(path, "w");
    if (!meta) return;
    fprintf(meta, "frame %llu pass %u color %llx depth %llx load %u\n",
            (unsigned long long)target_debug.frame, target_debug.number,
            (unsigned long long)target_debug.color_addr, (unsigned long long)target_debug.depth_addr, target_debug.color_load);
    VkSpan dst = vk_readback(total);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int i = 0; i < 3; i++) if (sizes[i]) {
        copies[i].bufferOffset += dst.off;
        vkCmdCopyImageToBuffer(cb, images[i]->image, VK_IMAGE_LAYOUT_GENERAL, dst.buf, 1, &copies[i]);
    }
    vk_readback_wait();
    for (int i = 0; i < 3; i++) if (sizes[i]) {
        snprintf(path, sizeof path, "%s-%s.raw", base, names[i]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((uint8_t *)dst.ptr + offsets[i], 1, sizes[i], f); fclose(f); }
        fprintf(meta, "%s %ux%u format %u level %u layer %u bytes %llu\n", names[i],
                copies[i].imageExtent.width, copies[i].imageExtent.height, images[i]->format,
                copies[i].imageSubresource.mipLevel, copies[i].imageSubresource.baseArrayLayer, (unsigned long long)sizes[i]);
    }
    fclose(meta);
    LOG("[target-debug] saved %s: color %llx depth %llx\n", base,
        (unsigned long long)target_debug.color_addr, (unsigned long long)target_debug.depth_addr);
}
