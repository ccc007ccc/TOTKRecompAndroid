// Invalid inputs must leave the output untouched. Layout sizes come from the existing CPU implementation.
#include "../runtime/nvn/vulkan/deswizzle_shader.h"
#include "../runtime/nvn/tegra.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "w"); assert(f);
    fputs(VK_DESWIZZLE_GLSL, f); assert(!fclose(f));
    const uint32_t invalid[][6] = {
        {0,1,1,0,0,0}, {3,1,1,0,0,0}, {4,0,1,0,0,0}, {4,1,0,0,0,0},
        {4,1,1,32,0,0}, {4,1,1,0,32,0}, {4,1,1,6,0,0}, {4,1,1,0,6,0},
        {UINT32_MAX-3,UINT32_MAX,UINT32_MAX,5,5,0}, {4,UINT32_MAX,1,0,0,0},
        {4,1,UINT32_MAX,0,0,0}, {4,1,1,0,0,UINT32_MAX}, {4,1,1,0,0,UINT32_MAX/4-126},
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        uint32_t pc[8], before[8]; memset(pc, 0xa5, sizeof pc); memcpy(before, pc, sizeof pc);
        uint64_t bytes = 123;
        const uint32_t *v = invalid[i];
        assert(!vk_deswizzle_params(v[0],v[1],v[2],v[3],v[4],v[5],pc,&bytes));
        assert(bytes == 123 && !memcmp(pc,before,sizeof pc));
    }
    uint32_t pc[8]; uint64_t bytes;
    assert(vk_deswizzle_params(64,8,1,0,0,0,pc,&bytes) && bytes == 512 && pc[3] == 128);
    assert(vk_deswizzle_params(4,1,1,0,0,7,pc,&bytes) && bytes == 512 && pc[0] == 7);
    assert(vk_deswizzle_params(4,1,1,0,0,UINT32_MAX/4-128,pc,&bytes));
    const FormatInfo fmt = {.bw=1,.bh=1,.bpb=4};
    unsigned checks = 0;
    for (uint32_t bh=0;bh<=5;++bh) for (uint32_t bd=0;bd<=5;++bd) {
        TexLayout t = {.width=37,.height=267,.depth=33,.levels=1,.layers=1,.fmt=&fmt,
                       .block_height_log2=bh,.block_depth_log2=bd};
        assert(vk_deswizzle_params(t.width*4,t.height,t.depth,bh,bd,0,pc,&bytes));
        assert(bytes == tegra_total_size(&t)); ++checks;
    }
    printf("PASS: %u CPU layout sizes and %zu invalid/overflow inputs\n",checks,sizeof invalid/sizeof *invalid);
}
