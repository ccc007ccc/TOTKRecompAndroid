// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <stdlib.h>
#include <string.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

// The largest power of two of GOBs, up to 16, that 1.5x the extent still fills.
// Count complete GOBs before choosing the power of two. Rounding up here turns
// 40 compressed rows into 8 GOBs instead of 7 and selects an oversized tile:
// TOTK's 160x160 warp-platform texture then samples padding as black rectangles.
static uint32_t pick_tile_log2(uint32_t units) {
    return units >= 16 ? 4 : units >= 8 ? 3 : units >= 4 ? 2 : units >= 2 ? 1 : 0;
}

uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    static int old = -1;  // LARECOMP_TEGRA_OLD_BH=1: the previous rule (GOB count rounded up to a power of two)
    if (old < 0) old = getenv("LARECOMP_TEGRA_OLD_BH") != NULL;
    if (old) {
        uint32_t gobs = div_up(height_in_blocks, 8);
        uint32_t log2 = 0;
        while ((1u << log2) < gobs && log2 < 4) log2++;
        return log2;
    }
    return pick_tile_log2((height_in_blocks + height_in_blocks / 2) / 8);
}

// 3D textures tile in depth only: one GOB high blocks (deko3d sets just the tile depth for them). With the 2D rule
// (4 GOBs high) BOTW's sky scattering LUT (256x32x16 RGBA16F) read scrambled and the sky came out brown.
void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    if (t->depth > 1) {
        t->block_height_log2 = 0;
        t->block_depth_log2 = pick_tile_log2(t->depth + t->depth / 2);
    } else {
        t->block_height_log2 = tegra_default_block_height_log2(hb);
        t->block_depth_log2 = 0;
    }
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size
        uint32_t bh = t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

// Offset of byte (x bytes, y rows, z slices) within a block-linear surface.
static inline uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2,
                                  uint32_t bd_log2, uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    // within the GOB: 64x8 bytes arranged in 16-byte sectors
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

static void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                         uint32_t bh_log2, uint32_t bd_log2, bool to_linear) {
    uint32_t row_bytes = wb * bpb;
    uint32_t width_gobs = (row_bytes + 63) / 64;
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    for (uint32_t z = 0; z < d; z++) {
        for (uint32_t y = 0; y < hb; y++) {
            uint8_t *lin = dst + ((uint64_t)z * hb + y) * row_bytes;
            const uint8_t *lin_src = src + ((uint64_t)z * hb + y) * row_bytes;
            // 16-byte runs are contiguous in both layouts
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint64_t o = gob_offset(x, y, z, width_gobs, bh_log2, bd_log2, blocks_rows);
                if (to_linear) memcpy(lin + x, src + o, n);
                else memcpy(dst + o, lin_src + x, n);
            }
        }
    }
}

void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, false);
}

static bool transfer_region(const TexLayout *t, uint32_t level, uint32_t layer, const int32_t r[6],
                            const uint8_t *src, uint8_t *dst, uint64_t stride, uint64_t row, uint64_t image, bool to_linear) {
    if (!t || !t->fmt || !t->fmt->bpb || !t->fmt->bw || !t->fmt->bh || !r || !dst || !src || level >= t->levels) return false;
    for (int i = 0; i < 6; i++) if (r[i] < 0) return false;
    if (!r[3] || !r[4] || !r[5]) return true;
    LevelLayout l = tegra_level(t, level);
    bool volume = t->depth > 1;
    uint32_t layers = t->layers ? t->layers : 1, limit = volume ? l.d : layers;
    if (layer >= layers || (volume && layer) || (uint32_t)r[0] > l.w || (uint32_t)r[3] > l.w - r[0] ||
        (uint32_t)r[1] > l.h || (uint32_t)r[4] > l.h - r[1] ||
        (uint64_t)layer + r[2] + r[5] > limit) return false;
    uint32_t bw = t->fmt->bw, bh = t->fmt->bh, bpb = t->fmt->bpb;
    // CPU texel transfers copy whole compressed blocks. A partial final block
    // need not end at the destination texture's edge: TOTK places 510-row BC4
    // mip images into a larger atlas. Round the extent up in blocks below.
    if (r[0] % bw || r[1] % bh) return false;
    uint32_t rows = div_up((uint32_t)r[4], bh), start_x = (uint32_t)r[0] / bw * bpb, start_y = (uint32_t)r[1] / bh;
    uint64_t bytes = (uint64_t)div_up((uint32_t)r[3], bw) * bpb;
    if (!row) row = bytes;
    if (row < bytes || row > UINT64_MAX / rows) return false;
    if (!image) image = row * rows;
    if (image < row * rows || image > SIZE_MAX / (uint32_t)r[5]) return false;
    if (stride && (level || volume || layers > 1 || stride < (uint64_t)l.wb * bpb)) return false;
    uint64_t layer_bytes = tegra_layer_size(t);
    uint32_t width_gobs = div_up(l.wb * bpb, 64);
    uint64_t blocks_rows = div_up(l.hb, 8u << l.block_height_log2);
    for (uint32_t z = 0; z < (uint32_t)r[5]; z++) {
        uint64_t base = l.offset + (volume ? 0 : (layer + r[2] + z) * layer_bytes);
        for (uint32_t y = 0; y < rows; y++) {
            uint64_t linear = z * image + y * row;
            if (stride) {
                uint64_t at = (start_y + y) * stride + start_x;
                if (to_linear) memcpy(dst + linear, src + at, bytes);
                else memcpy(dst + at, src + linear, bytes);
                continue;
            }
            for (uint64_t x = 0; x < bytes;) {
                uint32_t xb = start_x + (uint32_t)x, n = 16 - (xb & 15);
                if (n > bytes - x) n = (uint32_t)(bytes - x);
                uint64_t at = gob_offset(xb, start_y + y, volume ? r[2] + z : 0, width_gobs,
                                         l.block_height_log2, l.block_depth_log2, blocks_rows);
                if (to_linear) memcpy(dst + linear + x, src + base + at, n);
                else memcpy(dst + base + at, src + linear + x, n);
                x += n;
            }
        }
    }
    return true;
}

bool tegra_write_region(const TexLayout *t, uint32_t level, uint32_t layer, const int32_t r[6],
                        uint8_t *storage, uint64_t stride, const uint8_t *src, uint64_t row, uint64_t image) {
    return transfer_region(t, level, layer, r, src, storage, stride, row, image, false);
}

bool tegra_read_region(const TexLayout *t, uint32_t level, uint32_t layer, const int32_t r[6],
                       const uint8_t *storage, uint64_t stride, uint8_t *dst, uint64_t row, uint64_t image) {
    return transfer_region(t, level, layer, r, storage, dst, stride, row, image, true);
}
