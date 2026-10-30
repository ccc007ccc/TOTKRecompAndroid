#pragma once
#include "shader_cache.h"
// The disk loader/compiler supplies full semantic validation. Also verify that
// the entry point belongs to the requested NVN stage before using its interface.
static inline bool vk_shader_spirv_stage(const uint32_t *words,size_t count,int stage) {
    if((stage!=0 && stage!=1 && stage!=5) || !vk_shader_cache_framed(words,count))return false;
    unsigned model=stage==0?0:stage==1?4:5;bool found=false;
    for(size_t i=5;i<count;i+=words[i]>>16)if((words[i]&65535)==15) {
        if((words[i]>>16)<4 || words[i+1]!=model)return false;
        found=true;
    }
    return found;
}
