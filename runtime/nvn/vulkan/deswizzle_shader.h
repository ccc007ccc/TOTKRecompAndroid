// Block-linear (Tegra GOB) texels to linear rows, one 32-bit word per invocation in a grid-stride loop: tegra.c's
// gob_offset. Built in as "deswizzle.comp" (shaders.c) for deswizzle.c; tools/test_vk_deswizzle.c runs
// the same source against tegra_deswizzle. Push constants: the level's first word in the source binding, a linear
// row in words, rows (blocks), total words, GOBs per row, block height / depth (log2), block rows per slice.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VK_DESWIZZLE_GLSL \
    "#version 450\n" \
    "layout(local_size_x = 64) in;\n" \
    "layout(std430, set = 0, binding = 0) readonly buffer S { uint w[]; } src;\n" \
    "layout(std430, set = 0, binding = 1) writeonly buffer D { uint w[]; } dst;\n" \
    "layout(push_constant) uniform P { uint first, row_words, rows, words, width_gobs, bh, bd, blocks_rows; } pc;\n" \
    "void main() {\n" \
    "  uint block = 512u << (pc.bh + pc.bd);\n" \
    "  for (uint i = gl_GlobalInvocationID.x; i < pc.words; i += gl_NumWorkGroups.x * 64u) {\n" \
    "    uint x = (i % pc.row_words) * 4u, yz = i / pc.row_words, y = yz % pc.rows, z = yz / pc.rows;\n" \
    "    uint gy = y >> 3u, by = gy >> pc.bh, iy = gy & ((1u << pc.bh) - 1u);\n" \
    "    uint bz = z >> pc.bd, iz = z & ((1u << pc.bd) - 1u);\n" \
    "    uint xi = x & 63u, yi = y & 7u;\n" \
    "    uint o = ((bz * pc.blocks_rows + by) * pc.width_gobs + (x >> 6u)) * block + ((iz << pc.bh) | iy) * 512u +\n" \
    "             (((xi >> 5u) << 8u) | ((yi >> 1u) << 6u) | (((xi >> 4u) & 1u) << 5u) | ((yi & 1u) << 4u) | (xi & 15u));\n" \
    "    dst.w[i] = src.w[pc.first + (o >> 2u)];\n" \
    "  }\n" \
    "}\n"

// The push constants for one level (row bytes a multiple of 4, `first`: its word in the source binding) and the source
// bytes the shader reads - tegra.c's level_size. Reject invalid tile shifts, arithmetic overflow and levels that
// 32-bit byte offsets cannot cover. Check products before multiplying, including values wider than real textures.
static inline bool vk_deswizzle_params(uint32_t row, uint32_t hb, uint32_t d, uint32_t bh, uint32_t bd, uint32_t first,
                                       uint32_t pc[8], uint64_t *bytes) {
    if ((row & 3) || !row || !hb || !d || bh > 5 || bd > 5) return false;
    uint64_t width_gobs = ((uint64_t)row + 63) / 64, gob_rows = 8ull << bh;
    uint64_t blocks_rows = ((uint64_t)hb + gob_rows - 1) / gob_rows, gob_depth = 1ull << bd;
    uint64_t depth_blocks = ((uint64_t)d + gob_depth - 1) / gob_depth;
    uint64_t n = width_gobs * (512ull << (bh + bd));
    if (n > UINT32_MAX / blocks_rows) return false;
    n *= blocks_rows;
    if (n > UINT32_MAX / depth_blocks) return false;
    n *= depth_blocks;
    if ((uint64_t)first * 4 > UINT32_MAX - n) return false;
    uint64_t words = (uint64_t)(row / 4) * hb;
    if (words > (UINT32_MAX / 4) / d) return false;
    words *= d;
    pc[0] = first, pc[1] = row / 4, pc[2] = hb, pc[3] = (uint32_t)words;
    pc[4] = (uint32_t)width_gobs, pc[5] = bh, pc[6] = bd, pc[7] = (uint32_t)blocks_rows;
    *bytes = n;
    return true;
}
