// Tegra X1 texture layout: formats and block-linear (GOB) swizzling.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t bpb;     // bytes per block (per texel for uncompressed)
    uint8_t bw, bh;  // block dimensions in texels
    uint8_t kind;    // FMT_COLOR, FMT_DEPTH, ...
    const char *name;
} FormatInfo;

enum { FMT_COLOR = 0, FMT_DEPTH = 1, FMT_STENCIL = 2, FMT_DEPTH_STENCIL = 3, FMT_COMPRESSED = 4 };

const FormatInfo *nvn_format_info(uint32_t nvn_format);

typedef struct {
    uint32_t width, height, depth, layers, levels;  // depth: 3D depth; layers: array layers / cube faces
    const FormatInfo *fmt;
    uint32_t block_height_log2;  // GOBs per block (log2) for level 0
    uint32_t block_depth_log2;
} TexLayout;

typedef struct {
    uint64_t offset;              // from the start of the layer
    uint32_t w, h, d;             // texel dimensions
    uint32_t wb, hb;              // in blocks
    uint32_t block_height_log2, block_depth_log2;
} LevelLayout;

// Default block height for a texture (what the NVN driver picks), and the full storage layout.
uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks);
void tegra_layout_init(TexLayout *t);
LevelLayout tegra_level(const TexLayout *t, uint32_t level);
uint64_t tegra_layer_size(const TexLayout *t);
uint64_t tegra_total_size(const TexLayout *t);

// Copy a block-linear level slice into a linear buffer (row pitch = wb * bpb).
void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t block_height_log2, uint32_t block_depth_log2);
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2);
// Write a mip/subrectangle/array-layer range, preserving other texels and tile padding.
// linear_stride is zero for block-linear storage; source strides of zero mean tightly packed.
// Compressed origins must be block-aligned; extents include the complete final block.
bool tegra_write_region(const TexLayout *layout, uint32_t level, uint32_t layer, const int32_t region[6],
                        uint8_t *storage, uint64_t linear_stride, const uint8_t *source,
                        uint64_t source_row, uint64_t source_image);
// Read the same region from CPU storage without decoding compressed blocks or
// touching the destination's row/image padding. Zero strides mean tightly packed.
bool tegra_read_region(const TexLayout *layout, uint32_t level, uint32_t layer, const int32_t region[6],
                       const uint8_t *storage, uint64_t linear_stride, uint8_t *destination,
                       uint64_t destination_row, uint64_t destination_image);
