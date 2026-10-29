#include "../../diagnostics_build.h"
// Explicit diagnosis of a 2D composition draw, before its inputs are consumed.
// One submission/readback covers all supported inputs, and files are written before
// the shared readback buffer is reused. This changes timing; it is never on by default.
static void debug_fragment_inputs(Prog *pr) {
    const char *want = diag_getenv("LARECOMP_DUMP_FRAGMENT_INPUT");
    uint64_t frame = renderer_present_count();
    static uint64_t last_frame = ~0ull;
    if (!want || strtoull(want, NULL, 16) != pr->code[1] || frame == last_frame) return;
    last_frame = frame;
    TexBackend *images[8] = {0};
    uint64_t offsets[8] = {0}, sizes[8] = {0}, total = 0;
    mkdir("build/fragment-input", 0755);
    char path[128];
    snprintf(path, sizeof path, "build/fragment-input/%llu.txt", (unsigned long long)frame);
    FILE *meta = fopen(path, "w");
    if (!meta) return;
    fprintf(meta, "fs %llx frame %llu\n", (unsigned long long)pr->code[1], (unsigned long long)frame);
    for (int i = 0; i < pr->fi->ntex && i < 8; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        TexBackend *b = t ? t->backend : NULL;
        if (!b || b->layers != 1 || b->samples != VK_SAMPLE_COUNT_1_BIT) continue;
        uint32_t bpp = b->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 :
                       b->format == VK_FORMAT_R32_SFLOAT || b->format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 0;
        if (!bpp) continue;
        images[i] = b;
        offsets[i] = total;
        sizes[i] = (uint64_t)b->width * b->height * bpp;
        total += (sizes[i] + 255) & ~255ull;
        fprintf(meta, "tex%d %ux%u format %d nvn %x address %llx bytes %llu\n", i, b->width, b->height,
                b->format, t->format, (unsigned long long)t->addr, (unsigned long long)sizes[i]);
    }
    if (!total || total > (64ull << 20)) { fclose(meta); return; }
    vk_end_pass();
    VkSpan dst = vk_readback(total);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int i = 0; i < 8; i++) if (images[i]) {
        VkBufferImageCopy copy = {.bufferOffset = dst.off + offsets[i],
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {images[i]->width, images[i]->height, 1}};
        vkCmdCopyImageToBuffer(cb, images[i]->image, VK_IMAGE_LAYOUT_GENERAL, dst.buf, 1, &copy);
    }
    vk_readback_wait();
    for (int i = 0; i < 8; i++) if (images[i]) {
        snprintf(path, sizeof path, "build/fragment-input/%llu-tex%d.raw", (unsigned long long)frame, i);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((uint8_t *)dst.ptr + offsets[i], 1, sizes[i], f); fclose(f); }
    }
    for (int k = 3; k < 18; k++) if (pr->fi->cbuf_mask & (1u << k)) {
        Range r = st.ubuf[1][k - 3];
        if (!r.addr || !r.size || r.size > 0x10000) continue;
        snprintf(path, sizeof path, "build/fragment-input/%llu-cb%d.bin", (unsigned long long)frame, k);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((void *)(uintptr_t)r.addr, 1, r.size, f); fclose(f); }
        fprintf(meta, "cb%d address %llx bytes %llu\n", k, (unsigned long long)r.addr, (unsigned long long)r.size);
    }
    fclose(meta);
    LOG("[fragment-debug] saved inputs of %llx at frame %llu\n", (unsigned long long)pr->code[1], (unsigned long long)frame);
}
