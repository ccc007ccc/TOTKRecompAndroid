#pragma once
#include <stdbool.h>
#include <stdint.h>

// Match vk_view's sampled range: a zero count means all remaining levels/layers, not only layer zero.
static bool vk_sampled_view_overlaps(uint32_t levels, uint32_t layers, uint32_t lv0, uint32_t nlv,
                                     uint32_t ly0, uint32_t nly, uint32_t render_lv, uint32_t render_ly) {
    if (!levels || !layers) return false;
    if (lv0 >= levels) lv0 = levels - 1;
    if (ly0 >= layers) ly0 = layers - 1;
    if (!nlv || nlv > levels - lv0) nlv = levels - lv0;
    if (!nly || nly > layers - ly0) nly = layers - ly0;
    return render_lv >= lv0 && render_lv - lv0 < nlv && render_ly >= ly0 && render_ly - ly0 < nly;
}
