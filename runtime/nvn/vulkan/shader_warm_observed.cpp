#include "shader_warm_observed.hpp"
#include "shader_warm.h"
#include <mutex>
#include <cerrno>
extern "C" {
#include "../nvn.h"
}
namespace {
std::mutex observation_mutex;
FILE* observation_file=nullptr;
std::set<std::string> recorded;
size_t unflushed=0;
bool stage_key(const NvnShaderStage* stage,shader_warm::RawKey& key) {
    if(!stage || !stage->control || !stage->code_addr || !stage->const_offset || stage->const_offset>16u*1024*1024 ||
       stage->const_size>16u*1024*1024-stage->const_offset)return false;
    shader_archive::View head(stage->control,0x24);auto length=head.u32(0x20);
    if(head.u32(0)!=0x98761234 || (length!=0x880 && length!=0x7d1))return false;
    shader_archive::View control(stage->control,length);auto bytes=stage->const_offset+stage->const_size;
    if(control.u32(0x704)!=bytes || control.u32(0x714)!=stage->stage)return false;
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);CC_SHA256_Update(&hash,stage->control,length);
    CC_SHA256_Update(&hash,reinterpret_cast<const void*>(uintptr_t(stage->code_addr)),bytes);CC_SHA256_Final(key.data(),&hash);
    return true;
}
}
extern "C" void vk_shader_warm_observe_begin(const char* game) {
    std::lock_guard<std::mutex> lock(observation_mutex);
    if(observation_file)fclose(observation_file);observation_file=nullptr;recorded.clear();unflushed=0;
    if(!game)return;
    try {
        auto path=shader_warm::state_directory(game)+"/observed.tsv";
        auto recent=shader_warm::read_recent_game(game);shader_warm::compact_observed(path,recent);
        observation_file=fopen(path.c_str(),"ab");
        if(!observation_file)fprintf(stderr,"[shader-warm] cannot record recent states at %s: %s\n",path.c_str(),strerror(errno));
    } catch(const std::exception& error) {
        fprintf(stderr,"[shader-warm] cannot start observation log: %s\n",error.what());
    }
}
extern "C" void vk_shader_warm_observe_flush(void) {
    std::lock_guard<std::mutex> lock(observation_mutex);
    if(observation_file)fflush(observation_file);unflushed=0;
}
extern "C" void vk_shader_warm_observe_end(void) {
    std::lock_guard<std::mutex> lock(observation_mutex);
    if(observation_file)fclose(observation_file);observation_file=nullptr;recorded.clear();unflushed=0;
}
extern "C" void vk_shader_warm_observe(const NvnShaderStage* first,const NvnShaderStage* second,
                                      const ShaderKey* first_key,const ShaderKey* second_key) {
    std::lock_guard<std::mutex> lock(observation_mutex);
    if(!observation_file || !first_key)return;
    shader_warm::Request request;request.stage=second?shader_warm::Pair:5;
    if(!stage_key(first,request.first) || (second && (!second_key || !stage_key(second,request.second))))return;
    if(second) {
        if(first->stage!=0 || second->stage!=1)return;
        request.mirror=first_key->flip_x;
        std::copy_n(first_key->attr_kind,32,request.attributes.begin());std::copy_n(second_key->rt_kind,8,request.outputs.begin());
    } else {
        if(first->stage!=5)return;
        std::copy_n(first_key->image_kind,8,request.images.begin());
    }
    auto line=shader_warm::observed_line(request);
    if(!recorded.insert(line).second)return;
    // Observations arrive once per newly used program, not once per draw. Small
    // batched appends retain recent states without a per-frame filesystem walk.
    if(fwrite(line.data(),1,line.size(),observation_file)!=line.size() || fputc('\n',observation_file)==EOF) {
        fclose(observation_file);observation_file=nullptr;return;
    }
    if(++unflushed>=32){fflush(observation_file);unflushed=0;}
}
