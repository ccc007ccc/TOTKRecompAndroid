#pragma once
#include "shaders.h"
#ifdef __cplusplus
extern "C" {
#endif
uint8_t vk_shader_gather_bias_bits(void);
bool vk_shader_vertex_snap_on(void);
// Atomic renderer memory-path snapshot: bit 0 mapped, bit 1 CPU copy/miss log.
unsigned vk_shader_memory_modes(void);
#ifdef __cplusplus
}
#endif
