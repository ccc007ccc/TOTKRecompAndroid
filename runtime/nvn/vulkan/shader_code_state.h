// Render-thread write epochs for code already referenced by a program cache.
// NVN callers must fence in-flight work before rewriting/rebinding shader code.
#pragma once
#include "../nvn.h"
#include <stdlib.h>
#include <string.h>
typedef struct {uint64_t lo,hi;} ShaderCodeRange;
typedef struct {uint64_t epoch;ShaderCodeRange* ranges;size_t count,capacity;} ShaderCodeState;
static inline void shader_code_range(ShaderCodeState* state,uint64_t addr,uint64_t size) {
    if(!size || addr>UINT64_MAX-size)return;
    uint64_t end=addr+size;
    size_t first=0;
    while(first<state->count && state->ranges[first].hi<addr)++first;
    size_t last=first;
    while(last<state->count && state->ranges[last].lo<=end) {
        if(state->ranges[last].lo<addr)addr=state->ranges[last].lo;
        if(state->ranges[last].hi>end)end=state->ranges[last].hi;
        ++last;
    }
    if(first==last && state->count==state->capacity) {
        size_t capacity=state->capacity ? state->capacity*2 : 32;
        ShaderCodeRange* ranges=realloc(state->ranges,capacity*sizeof *ranges);
        if(!ranges)FATAL("shader code range allocation failed\n");
        state->ranges=ranges;state->capacity=capacity;
    }
    memmove(state->ranges+first+1,state->ranges+last,(state->count-last)*sizeof *state->ranges);
    state->ranges[first]=(ShaderCodeRange){addr,end};state->count=state->count-(last-first)+1;
}
static inline void shader_code_watch(ShaderCodeState* state,const NvnShaderStage* stage) {
    uint64_t bytes=stage->const_offset ? (uint64_t)stage->const_offset+stage->const_size : 0x10000;
    if(stage->code_addr)shader_code_range(state,stage->code_addr,bytes);
    if(stage->control)shader_code_range(state,(uintptr_t)stage->control,0x880);
}
static inline void shader_code_written(ShaderCodeState* state,uint64_t addr,uint64_t size) {
    if(!size || addr>UINT64_MAX-size)return;
    size_t lo=0,hi=state->count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(state->ranges[mid].hi<=addr)lo=mid+1;else hi=mid;}
    if(lo<state->count && state->ranges[lo].lo<addr+size)++state->epoch;
}
static inline void shader_code_reset(ShaderCodeState* state) {
    free(state->ranges);memset(state,0,sizeof *state);
}
