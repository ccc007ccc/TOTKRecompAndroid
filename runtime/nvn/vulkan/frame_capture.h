// LARECOMP_CAPTURE_ATTACHMENTS=1: capture render-target transitions in debug
// frames, including every MRT slot and the selected mip/layer. Copies are
// queued at the transition; CPU waiting and file I/O happen once at present.
// This still changes GPU timing and is unsuitable for performance measurements.
#define CAPTURE_BYTES (512ull << 20)
static struct {
    VkBuffer buffer;
    VmaAllocation memory;
    uint8_t *mapped;
    uint64_t used;
    uint32_t count, group, dropped;
    bool dirty;
    struct {
        uint64_t offset, size, address;
        uint32_t group, slot, width, height, level, layer, format, aspect;
    } entries[1024];
} frame_capture;

static bool capture_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LARECOMP_CAPTURE_ATTACHMENTS") != NULL;
    return enabled && g_debug_frame;
}

static uint32_t capture_texel_bytes(VkFormat f, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return 1;
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

static void diagnostic_targets(const RtState *rt) {
    if(!vk_diag_dirty())return;
    vk_end_pass();
    for(int slot=0;slot<10;slot++) {
        NvnTexture *t=slot<8?(slot<rt->ncolor?rt->color[slot]:NULL):rt->depth;
        TexBackend *b=t?t->backend:NULL;
        VkImageAspectFlags aspect=slot<8?VK_IMAGE_ASPECT_COLOR_BIT:slot==8?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT;
        if(!b||!(b->aspect&aspect))continue;
        const NvnTextureView *v=slot<8?&rt->cview[slot]:&rt->dview;
        bool has=slot<8?rt->has_cview[slot]:rt->has_dview;
        unsigned level=has&&(v->setmask&1)?v->base_level:0;
        unsigned layer=has&&(v->setmask&2)?v->min_layer:0;
        vk_diag_image(b,t->addr,(unsigned)slot,level,layer,aspect);
    }
    vk_diag_next_group();
}

static void capture_targets(const RtState *rt) {
    diagnostic_targets(rt);
    if (!capture_enabled() || !frame_capture.dirty) return;
    frame_capture.dirty = false;
    vk_end_pass();
    uint32_t group = frame_capture.group++;
    LOG("[capture] group %u frame %llu: %d color targets\n", group,
        (unsigned long long)renderer_present_count(), rt->ncolor);
    VkCommandBuffer command = VK_NULL_HANDLE;
    for (int slot = 0; slot < 10; slot++) {
        NvnTexture *t = slot < 8 ? (slot < rt->ncolor ? rt->color[slot] : NULL) : rt->depth;
        TexBackend *b = t ? t->backend : NULL;
        VkImageAspectFlags aspect = slot < 8 ? VK_IMAGE_ASPECT_COLOR_BIT :
                                   slot == 8 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!b || !(b->aspect & aspect) || b->samples != 1 || b->depth != 1) continue;
        const NvnTextureView *v = slot < 8 ? &rt->cview[slot] : &rt->dview;
        bool has = slot < 8 ? rt->has_cview[slot] : rt->has_dview;
        uint32_t level = has && (v->setmask & 1) ? v->base_level : 0;
        uint32_t layer = has && (v->setmask & 2) ? v->min_layer : 0;
        if (level >= b->levels || layer >= b->layers) continue;
        // Duplicate MRT bindings share one set of bytes; preserve the first slot.
        bool duplicate = false;
        for (int j = 0; j < slot && j < 8; j++) {
            if (slot >= 8 || !rt->color[j] || rt->color[j]->backend != b) continue;
            uint32_t jl = rt->has_cview[j] && (rt->cview[j].setmask & 1) ? rt->cview[j].base_level : 0;
            uint32_t ja = rt->has_cview[j] && (rt->cview[j].setmask & 2) ? rt->cview[j].min_layer : 0;
            if (jl == level && ja == layer) duplicate = true;
        }
        if (duplicate) continue;
        uint32_t bytes = capture_texel_bytes(b->format, aspect);
        if (!bytes) { frame_capture.dropped++; continue; }
        uint32_t width = b->width >> level, height = b->height >> level;
        if (!width) width = 1;
        if (!height) height = 1;
        uint64_t size = (uint64_t)width * height * bytes;
        uint64_t offset = (frame_capture.used + 255) & ~255ull;
        if (size > CAPTURE_BYTES - offset || frame_capture.count == 1024) {
            frame_capture.dropped++;
            continue;
        }
        if (!frame_capture.buffer) {
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = CAPTURE_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo info;
            VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &frame_capture.buffer, &frame_capture.memory, &info));
            frame_capture.mapped = info.pMappedData;
        }
        if (!command) { command = vk_cmd(); vk_full_barrier(command); }
        VkBufferImageCopy copy = {.bufferOffset = offset,
            .imageSubresource = {aspect, level, layer, 1}, .imageExtent = {width, height, 1}};
        vkCmdCopyImageToBuffer(command, b->image, VK_IMAGE_LAYOUT_GENERAL, frame_capture.buffer, 1, &copy);
        uint32_t n = frame_capture.count++;
        frame_capture.entries[n].offset = offset;
        frame_capture.entries[n].size = size;
        frame_capture.entries[n].address = t->addr;
        frame_capture.entries[n].group = group;
        frame_capture.entries[n].slot = slot;
        frame_capture.entries[n].width = width;
        frame_capture.entries[n].height = height;
        frame_capture.entries[n].level = level;
        frame_capture.entries[n].layer = layer;
        frame_capture.entries[n].format = b->format;
        frame_capture.entries[n].aspect = aspect;
        frame_capture.used = offset + size;
    }
    if (command) vk_full_barrier(command);  // subsequent draws may overwrite a copied image
}

void vk_capture_finish(NvnTexture *presented) {
    diagnostic_targets(&st.rt);
    if (!capture_enabled() && !frame_capture.buffer) return;
    capture_targets(&st.rt);
    // Include the presented image under the same frame number as its draws.
    RtState final = {.ncolor = 1, .color = {presented}};
    frame_capture.dirty = true;
    capture_targets(&final);
    if (frame_capture.buffer) {
        vk_readback_barrier(vk_cmd());
        renderer_flush();
        VK_CHECK(vmaInvalidateAllocation(g_vk.vma, frame_capture.memory, 0, frame_capture.used));
        mkdir("build/capture", 0755);
        char dir[128], path[192];
        snprintf(dir, sizeof dir, "build/capture/%llu", (unsigned long long)renderer_present_count());
        mkdir(dir, 0755);
        snprintf(path, sizeof path, "%s/manifest.tsv", dir);
        FILE *meta = fopen(path, "w");
        if (meta) {
            fprintf(meta, "file\tgroup\tslot\taddress\twidth\theight\tformat\taspect\tlevel\tlayer\tbytes\n");
            for (uint32_t i = 0; i < frame_capture.count; i++) {
                snprintf(path, sizeof path, "%s/%03u-%u.raw", dir, frame_capture.entries[i].group, frame_capture.entries[i].slot);
                FILE *file = fopen(path, "wb");
                size_t written = 0;
                if (file) {
                    written = fwrite(frame_capture.mapped + frame_capture.entries[i].offset, 1, frame_capture.entries[i].size, file);
                    fclose(file);
                }
                fprintf(meta, "%03u-%u.raw\t%u\t%u\t%llx\t%u\t%u\t%u\t%u\t%u\t%u\t%zu\n",
                    frame_capture.entries[i].group, frame_capture.entries[i].slot,
                    frame_capture.entries[i].group, frame_capture.entries[i].slot,
                    (unsigned long long)frame_capture.entries[i].address,
                    frame_capture.entries[i].width, frame_capture.entries[i].height,
                    frame_capture.entries[i].format, frame_capture.entries[i].aspect,
                    frame_capture.entries[i].level, frame_capture.entries[i].layer, written);
            }
            fclose(meta);
        }
        LOG("[capture] frame %llu: %u images, %llu MB, %u omitted -> %s\n",
            (unsigned long long)renderer_present_count(), frame_capture.count,
            (unsigned long long)(frame_capture.used >> 20), frame_capture.dropped, dir);
        vmaDestroyBuffer(g_vk.vma, frame_capture.buffer, frame_capture.memory);
    }
    memset(&frame_capture, 0, sizeof frame_capture);
}
