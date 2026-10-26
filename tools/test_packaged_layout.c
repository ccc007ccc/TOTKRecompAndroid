// Builder ABI and fixed GOB boundaries, independently documented by TotK-Native's SDK checks.
#define _GNU_SOURCE
#define __ANDROID__ 1  // Match the Android-only Settings and NVN service configuration.
#include <assert.h>
#include "../runtime/nvn/objects.c"
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *format, ...) { (void)format; abort(); }
static Cpu cpu;
static uint8_t builder[0x80];
static void set_layout(uint64_t *layout) {
    cpu.x[0] = (uintptr_t)builder; cpu.x[1] = (uintptr_t)layout;
    hle_nvnTextureBuilderSetPackagedTextureLayout(&cpu);
}
int main(void) {
    cpu.x[0] = (uintptr_t)builder; hle_nvnTextureBuilderSetDefaults(&cpu);
    G(uint32_t, (uintptr_t)builder, 0xc) = 1; // 2D R16, all mip levels
    G(uint32_t, (uintptr_t)builder, 0x10) = 1000;
    G(uint32_t, (uintptr_t)builder, 0x14) = 800;
    G(uint32_t, (uintptr_t)builder, 0x1c) = 10;
    G(uint32_t, (uintptr_t)builder, 0x20) = 6;
    uint64_t layout = 0x0001000700000004ull; set_layout(&layout);
    assert(builder_storage_size((uintptr_t)builder) == 2535936);
    TexLayout t = {0}; builder_layout((uintptr_t)builder, &t);
    uint64_t offsets[] = {0,1835008,2359296,2490368,2523136,2531328,2533376,2534400,2534912,2535424};
    for (unsigned i = 0; i < 10; i++) assert(tegra_level(&t, i).offset == offsets[i]);
    uint64_t output = 0xa5a5a5a5a5a5a5a5ull;
    cpu.x[0] = (uintptr_t)builder; cpu.x[1] = (uintptr_t)&output;
    hle_nvnTextureBuilderGetPackagedTextureLayout(&cpu);
    assert(cpu.x[0] == 1 && output == 0xa5a5a5a500000004ull);
    layout = 0xdeadbeeffffff004ull; set_layout(&layout);
    assert(builder_storage_size((uintptr_t)builder) == 2535936);
    G(uint32_t, (uintptr_t)builder, 0xc) = 2;
    G(uint32_t, (uintptr_t)builder, 0x10) = 64;
    G(uint32_t, (uintptr_t)builder, 0x14) = 64;
    G(uint32_t, (uintptr_t)builder, 0x18) = 64;
    G(uint32_t, (uintptr_t)builder, 0x1c) = 7;
    G(uint32_t, (uintptr_t)builder, 0x20) = 0x49; // BC4
    for (unsigned depth = 0; depth <= 4; depth += 2) {
        layout = depth << 4; set_layout(&layout);
        t = (TexLayout){0}; builder_layout((uintptr_t)builder, &t);
        assert(t.block_height_log2 == 0 && t.block_depth_log2 == depth);
        assert(tegra_total_size(&t) == 163328);
        uint64_t voxel = 512ull << depth;
        uint8_t *tiled = calloc(1, tegra_total_size(&t)), *linear = calloc(1, 16*16*64*8);
        assert(tiled && linear);
        tiled[voxel] = 0x5a;
        tegra_deswizzle(tiled, linear, 16, 16, 64, 8, 0, depth);
        assert(linear[64] == 0x5a); // second GOB in x, y=0,z=0
        free(tiled); free(linear);
    }
    set_layout(NULL); t = (TexLayout){0}; builder_layout((uintptr_t)builder, &t);
    assert(t.block_depth_log2 == 4); // clearing an explicit zero restores automatic selection
    NvnTexture texture = {.width = 64, .height = 64, .format = 0x49, .layout = t};
    texture.layout.depth = 1; texture.layout.layers = 4; tegra_layout_init(&texture.layout);
    uint64_t object[4] = {(uintptr_t)&texture, 0, 0, 0x123456};
    NvnTextureView view = {.setmask = 3, .base_level = 2, .num_levels = 1, .min_layer = 3, .num_layers = 1};
    cpu.x[0] = (uintptr_t)object; cpu.x[1] = (uintptr_t)&view;
    hle_nvnTextureGetViewOffset(&cpu);
    assert(cpu.x[0] == 3 * tegra_layer_size(&texture.layout) + tegra_level(&texture.layout, 2).offset);
    cpu.x[0] = (uintptr_t)object; hle_nvnTextureGetMemoryPool(&cpu); assert(cpu.x[0] == 0x123456);
    puts("packaged texture tests passed: builder ABI, explicit zero, height/depth, mip offsets and GOB boundaries");
}
