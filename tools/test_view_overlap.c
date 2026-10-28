#include "../runtime/nvn/vulkan/view_overlap.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
int main(void) {
    // The terrain clipmap samples the full 8-layer, 4-level array while filling its higher layers and mips.
    for (unsigned lv = 0; lv < 4; lv++) for (unsigned ly = 0; ly < 8; ly++)
        CHECK(vk_sampled_view_overlaps(4, 8, 0, 0, 0, 0, lv, ly));
    CHECK(!vk_sampled_view_overlaps(4, 8, 0, 1, 2, 1, 0, 3)); // disjoint layer
    CHECK(vk_sampled_view_overlaps(4, 8, 0, 1, 2, 1, 0, 2));
    CHECK(!vk_sampled_view_overlaps(4, 8, 0, 1, 0, 0, 1, 0)); // disjoint mip
    CHECK(vk_sampled_view_overlaps(4, 8, 1, 2, 3, 2, 2, 4));
    CHECK(!vk_sampled_view_overlaps(4, 8, 1, 2, 3, 2, 3, 4));
    CHECK(!vk_sampled_view_overlaps(4, 8, 1, 2, 3, 2, 2, 5));
    CHECK(vk_sampled_view_overlaps(4, 8, 3, UINT32_MAX, 7, UINT32_MAX, 3, 7)); // count clamp, no overflow
    CHECK(!vk_sampled_view_overlaps(4, 8, 0, 0, 0, 0, 4, 0));
    CHECK(!vk_sampled_view_overlaps(4, 8, 0, 0, 0, 0, 0, 8));
    CHECK(!vk_sampled_view_overlaps(0, 8, 0, 0, 0, 0, 0, 0));
    return 0;
}
