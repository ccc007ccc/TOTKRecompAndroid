#include "shader_keys.h"
#include "shader_config.h"
#include "shader_prepare.h"
#include <string.h>
#include <stdlib.h>
void vk_shader_graphics_key_defaults(ShaderKey *v,ShaderKey *f,bool flip_y,bool depth_minus_one) {
    memset(v,0,sizeof *v);memset(f,0,sizeof *f);
    v->glsl=f->glsl=1;v->gather_bias_bits=f->gather_bias_bits=vk_shader_prepare_gather();
    v->vertex_snap=vk_shader_prepare_vertex_snap();
    v->flip_y=flip_y;v->depth_minus_one=depth_minus_one;
}
void vk_shader_fragment_depth_key(ShaderKey *key,bool writable) {
#ifdef __ANDROID__
    key->no_ds_write=(vk_shader_memory_modes()&2) && !getenv("LARECOMP_NO_GMISS") && !writable;
#else
    (void)key;(void)writable;
#endif
}
void vk_shader_compute_key(ShaderKey *key,uint16_t kinds) {
    memset(key,0,sizeof *key);key->glsl=1;key->gather_bias_bits=vk_shader_prepare_gather();
    key->gmem_linear_lookup=vk_shader_prepare_compute_linear();
    for(unsigned i=0;i<NVN_IMAGE_BINDINGS;++i)key->image_kind[i]=(kinds>>(2*i))&3;
}
void vk_shader_key_canonical(ShaderKey *key,int stage) {
    if(stage!=STAGE_VERTEX){key->flip_y=false;key->depth_minus_one=false;}
    // flip_x also changes fragment attribute loads and the rscale declaration.
}
