// Opt-in capture of one actual vertex draw. Included by exec.c only.
// The transient buffer belongs to that submission; it is read back after the draw
// and released by the existing ring. Normal rendering allocates nothing here.
#define VERTEX_DEBUG_BINDING 127
#define VERTEX_DEBUG_RECORDS 32768u
#define VERTEX_DEBUG_WORDS 72u  // vertex/instance ids, clip position, 16 input attributes

static VkSpan vertex_debug_pending;
static bool vertex_debug_saved;
static uint64_t vertex_debug_saved_frame = ~0ull;
static VkDescriptorBufferInfo vertex_debug_cbufs[18];
static uint32_t vertex_debug_dynamic_cbufs;

static bool vertex_debug_selected(const char *want, uint64_t code, bool compiling) {
    if (!want) return false;
    if (!strncmp(want, "rt:", 3)) {
        // Shader addresses for streamed effects vary between boots. Instrument
        // vertex stages, but enable the buffer only for this target's first draw.
        unsigned w, h, format;
        if (sscanf(want + 3, "%ux%u:%x", &w, &h, &format) != 3) return false;
        if (compiling) return true;
        NvnTexture *t = st.rt.ncolor == 1 ? st.rt.color[0] : NULL;
        return t && t->width == w && t->height == h && t->format == format;
    }
    return strtoull(want, NULL, 16) == code;
}

static void vertex_debug_directory(char *path, size_t size) {
    mkdir("build/vertex-input", 0755);
    if (getenv("LARECOMP_DUMP_VERTEX_FRAMES")) {
        snprintf(path, size, "build/vertex-input/%llu", (unsigned long long)renderer_present_count());
        mkdir(path, 0755);
    } else snprintf(path, size, "build/vertex-input");
}

static void vertex_debug_instrument(ShaderInfo *si, const ShaderKey *key, uint64_t code) {
    const char *want = getenv("LARECOMP_DUMP_VERTEX_OUTPUT");
    if (!vertex_debug_selected(want, code, true)) return;
    VkPhysicalDeviceFeatures caps;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &caps);
    if (!caps.vertexPipelineStoresAndAtomics) {
        LOG("[vertex-debug] vertex storage writes unsupported\n");
        return;
    }
    char *main = strstr(si->msl, "void main()"), *end = strrchr(si->msl, '}');
    if (!main || !end || main >= end) return;
    const char *decl = "layout(std430,set=0,binding=127) buffer VertexDebug { "
                       "uint enabled; uint count; uint capacity; uint stride; uvec4 data[]; } vertex_debug;\n";
    // Optional two shader-visible uniform words in the record's reserved fields.
    // Example: 1:24,9:74 means cb1 byte 0x24 and cb9 byte 0x74. Unlike a later
    // buffer copy, these loads observe the bindings of this vertex invocation.
    char probes[2][64] = {"0u", "0u"};
    const char *probe = getenv("LARECOMP_VERTEX_PROBE");
    unsigned bank[2], offset[2];
    if (probe && sscanf(probe, "%u:%x,%u:%x", &bank[0], &offset[0], &bank[1], &offset[1]) == 4) {
        for (int i = 0; i < 2; i++)
            if (bank[i] < 18 && (si->cbuf_mask & (1u << bank[i])) && !(offset[i] & 3) &&
                offset[i] < si->cbuf_size[bank[i]])
                snprintf(probes[i], sizeof probes[i], "cb%u[%u][%u]", bank[i], offset[i] / 16, (offset[i] / 4) & 3);
    }
    char body[4096];
    int n = snprintf(body, sizeof body,
        "\nif (vertex_debug.enabled != 0u) { uint slot = atomicAdd(vertex_debug.count, 1u);\n"
        "if (slot < vertex_debug.capacity) { uint base = slot * 18u;\n"
        "vertex_debug.data[base] = uvec4(uint(gl_VertexIndex), uint(gl_InstanceIndex), %s, %s);\n"
        "vertex_debug.data[base+1u] = floatBitsToUint(gl_Position);\n", probes[0], probes[1]);
    for (unsigned i = 0; i < 16; i++) if (si->attr_mask & (1u << i))
        n += snprintf(body + n, sizeof body - n, "vertex_debug.data[base+%uu] = %s(a%u);\n",
                      i + 2, key->attr_kind[i] ? "uvec4" : "floatBitsToUint", i);
    n += snprintf(body + n, sizeof body - n, "} }\n");
    size_t size = strlen(si->msl) + strlen(decl) + (size_t)n + 1;
    char *source = malloc(size);
    if (!source) return;
    snprintf(source, size, "%.*s%s%.*s%s%s", (int)(main - si->msl), si->msl, decl,
             (int)(end - main), main, body, end);
    free(si->msl);
    si->msl = source;
    LOG("[vertex-debug] instrumented VS %llx\n", (unsigned long long)code);
}

static bool vertex_debug_capturing(void) {
    bool multi = getenv("LARECOMP_DUMP_VERTEX_FRAMES") != NULL;
    return g_debug_frame && (multi ? vertex_debug_saved_frame != renderer_present_count() : !vertex_debug_saved) &&
           vertex_debug_selected(getenv("LARECOMP_DUMP_VERTEX_OUTPUT"), st.program->stages[0].code_addr, false);
}

static VkDescriptorBufferInfo vertex_debug_info(void) {
    bool capture = vertex_debug_capturing();
    uint64_t bytes = capture ? 16ull + VERTEX_DEBUG_RECORDS * VERTEX_DEBUG_WORDS * 4ull : 16;
    VkSpan span = vk_transient(bytes, 256);
    uint32_t header[] = {capture, 0, VERTEX_DEBUG_RECORDS, VERTEX_DEBUG_WORDS};
    memcpy(span.ptr, header, sizeof header);
    if (capture) vertex_debug_pending = span;
    return (VkDescriptorBufferInfo){span.buf, span.off, bytes};
}

static void vertex_debug_finish(void) {
    if (!vertex_debug_pending.buf) return;
    const uint64_t bytes = 16ull + VERTEX_DEBUG_RECORDS * VERTEX_DEBUG_WORDS * 4ull;
    vk_end_pass();
    uint64_t offsets[18] = {0}, total = (bytes + 255) & ~255ull;
    for (int k = 0; k < 18; k++) {
        VkDescriptorBufferInfo *b = &vertex_debug_cbufs[k];
        if (!b->buffer || !b->range || b->range > 0x10000) continue;
        offsets[k] = total;
        total += (b->range + 255) & ~255ull;
    }
    VkSpan dst = vk_readback(total);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferCopy copy = {vertex_debug_pending.off, dst.off, bytes};
    vkCmdCopyBuffer(cb, vertex_debug_pending.buf, dst.buf, 1, &copy);
    for (int k = 0; k < 18; k++) if (offsets[k]) {
        VkDescriptorBufferInfo *b = &vertex_debug_cbufs[k];
        VkBufferCopy uniform = {b->offset, dst.off + offsets[k], b->range};
        vkCmdCopyBuffer(cb, b->buffer, dst.buf, 1, &uniform);
    }
    vk_readback_wait();  // only this explicit diagnostic waits for its GPU consumer
    uint32_t count = ((const uint32_t *)dst.ptr)[1];
    uint32_t kept = count < VERTEX_DEBUG_RECORDS ? count : VERTEX_DEBUG_RECORDS;
    char dir[128], path[160];
    vertex_debug_directory(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/gpu-vertices.bin", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        size_t size = 16 + (size_t)kept * VERTEX_DEBUG_WORDS * 4;
        if (fwrite(dst.ptr, 1, size, f) == size)
            LOG("[vertex-debug] saved %u of %u vertex invocations\n", kept, count);
        fclose(f);
    }
    for (int k = 0; k < 18; k++) if (offsets[k]) {
        snprintf(path, sizeof path, "%s/gpu-cb%d.bin", dir, k);
        f = fopen(path, "wb");
        if (f) { fwrite((uint8_t *)dst.ptr + offsets[k], 1, vertex_debug_cbufs[k].range, f); fclose(f); }
    }
    snprintf(path, sizeof path, "%s/bindings.txt", dir);
    f = fopen(path, "w");
    if (f) {
        const char *probe = getenv("LARECOMP_VERTEX_PROBE");
        if (probe) fprintf(f, "record words 2,3: %s (bank:hex-byte-offset)\n", probe);
        for (int k = 0; k < 18; k++) if (offsets[k]) {
            const VkDescriptorBufferInfo *b = &vertex_debug_cbufs[k];
            fprintf(f, "cb%d %s buffer %llx offset %llu range %llu\n", k,
                vertex_debug_dynamic_cbufs & (1u << k) ? "dynamic" : "static",
                (unsigned long long)(uintptr_t)b->buffer, (unsigned long long)b->offset, (unsigned long long)b->range);
        }
        fclose(f);
    }
#ifdef __ANDROID__
    debug_global_inputs();
#endif
    memset(vertex_debug_cbufs, 0, sizeof vertex_debug_cbufs);
    vertex_debug_dynamic_cbufs = 0;
    vertex_debug_pending = (VkSpan){0};
    vertex_debug_saved = true;
    vertex_debug_saved_frame = renderer_present_count();
}
