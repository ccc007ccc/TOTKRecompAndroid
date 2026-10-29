// The formats a mutable color image may be viewed as (VkImageFormatListCreateInfo): every uncompressed format
// textures.c's fmt_of() maps an NVN format to, by Vulkan compatibility class - NVN texture views, raw sRGB views and
// R32 surface loads stay within one. Turnip drops UBWC from a MUTABLE_FORMAT image without such a list
// (tu_image.cc: format_list_ubwc_possible); Adreno 7xx gen3 / 8xx compress all UBWC-capable formats compatibly, so a
// list of those keeps it. E5B9G9R9 can't be compressed: no list for it, and none of the other 32-bit formats list it.
#pragma once
#include <vulkan/vulkan.h>

static inline uint32_t vk_image_view_formats(VkFormat f, const VkFormat **list) {
    static const VkFormat c8[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT};
    static const VkFormat c16[] = {
        VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    static const VkFormat c32[] = {
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    static const VkFormat c64[] = {
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT};
    static const VkFormat c128[] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
    static const struct { const VkFormat *f; uint32_t n; } classes[] = {
        {c8, sizeof c8 / sizeof *c8}, {c16, sizeof c16 / sizeof *c16}, {c32, sizeof c32 / sizeof *c32},
        {c64, sizeof c64 / sizeof *c64}, {c128, sizeof c128 / sizeof *c128}};
    for (uint32_t c = 0; c < sizeof classes / sizeof *classes; c++)
        for (uint32_t i = 0; i < classes[c].n; i++)
            if (classes[c].f[i] == f) { *list = classes[c].f; return classes[c].n; }
    *list = NULL;
    return 0;
}
