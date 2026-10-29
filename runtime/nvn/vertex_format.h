// Input type must match the format actually bound by the Vulkan pipeline.
#pragma once
#include <stdint.h>
enum { NVN_VERTEX_FLOAT=0, NVN_VERTEX_SINT=1, NVN_VERTEX_UINT=2, NVN_VERTEX_UNKNOWN=3, NVN_VERTEX_BINDINGS=16 };
static inline unsigned nvn_vertex_format_kind(uint32_t format) {
    switch(format) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E: return NVN_VERTEX_UINT;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30: return NVN_VERTEX_SINT;
    default: return NVN_VERTEX_FLOAT;
    }
}
static inline unsigned nvn_vertex_state_kind(uint32_t packed) {
    // Invalid/unbound states use the renderer's (0,0,0,1) float constant stream.
    if(!(packed&255u) || ((packed>>8)&31u)>=NVN_VERTEX_BINDINGS)return NVN_VERTEX_FLOAT;
    return nvn_vertex_format_kind(packed&255u);
}
// BFRES nn::gfx format IDs differ from NVN IDs. Unknown formats stay open.
static inline unsigned bfres_vertex_format_kind(uint16_t format) {
    switch(format) {
    case 0x0302: case 0x0309: case 0x030a: case 0x030b: case 0x0315: return NVN_VERTEX_UINT;
    case 0x0102: case 0x0109: case 0x010b: case 0x0112:
    case 0x0209: case 0x020b: case 0x020e: case 0x0212:
    case 0x0512: case 0x0515: case 0x0517: case 0x0518: return NVN_VERTEX_FLOAT;
    default: return NVN_VERTEX_UNKNOWN;
    }
}
