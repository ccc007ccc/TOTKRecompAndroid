#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "../maxwell/shader.h"
void vk_shader_graphics_key_defaults(ShaderKey *vertex,ShaderKey *fragment,bool flip_y,bool depth_minus_one);
void vk_shader_fragment_depth_key(ShaderKey *fragment,bool depth_or_stencil_write);
void vk_shader_compute_key(ShaderKey *key,uint16_t image_kinds);
// Normalize only stage-independent clip flags proved unused by the translator.
void vk_shader_key_canonical(ShaderKey *key,int stage);
#ifdef __cplusplus
}
#endif
