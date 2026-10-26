// NVN 55.15 control layout, checked against TotK-Native's shader_binary.cpp.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { uint32_t stage, entry, constants, size; } NvnShaderControl;
static inline uint32_t nvn_control_word(const uint8_t *p, size_t offset) {
    uint32_t value; memcpy(&value, p + offset, 4); return value;
}
static inline bool nvn_shader_control(const uint8_t *control, size_t bytes, NvnShaderControl *out) {
    if (!control || bytes < 0x880 || nvn_control_word(control, 0) != 0x98761234 ||
        nvn_control_word(control, 0x20) != 0x880) return false;
    NvnShaderControl c = {nvn_control_word(control, 0x714), nvn_control_word(control, 0x708),
                         nvn_control_word(control, 0x700), nvn_control_word(control, 0x704)};
    if (c.stage >= 6 || c.entry != (c.stage == 5 ? 0x100u : 0x30u) || c.entry >= c.constants ||
        c.constants > c.size || c.size > 16u * 1024 * 1024 || (c.constants & 7) || (c.size & 7) ||
        (c.stage != 5 && c.constants - c.entry < 0x58)) return false;
    *out = c;
    return true;
}
