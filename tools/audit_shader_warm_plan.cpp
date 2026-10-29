// Count the actual observed-state source plan without compiling any SPIR-V.
#include "../runtime/nvn/vulkan/shader_warm_plan.hpp"
#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
#include "../runtime/nvn/vulkan/shader_prepare.h"
VkG g_vk;
void rt_fatal_trace(void){abort();}
#ifdef __ANDROID__
void rt_fatal_message(const char* format,...){va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);abort();}
#endif
}
int main(int argc,char** argv) {
    if(argc<3){std::cerr<<"usage: audit_shader_warm_plan ROMFS MANIFEST [--cache ROOT] [--title] [--keys OUTPUT]\n";return 2;}
    try {
        auto image=std::filesystem::absolute(argv[1]),manifest=std::filesystem::absolute(argv[2]);
        std::filesystem::path cache,keys_path;bool title=false;
        for(int i=3;i<argc;++i) {
            if(!strcmp(argv[i],"--title"))title=true;
            else if(!strcmp(argv[i],"--cache") && i+1<argc)cache=std::filesystem::absolute(argv[++i]);
            else if(!strcmp(argv[i],"--keys") && i+1<argc)keys_path=std::filesystem::absolute(argv[++i]);
            else throw std::runtime_error("invalid audit option");
        }
        if(!cache.empty() && chdir(cache.c_str()))throw std::runtime_error("cannot enter cache root");
        std::ofstream keys_file;
        if(!keys_path.empty()){keys_file.open(keys_path);if(!keys_file)throw std::runtime_error("cannot create key inventory");}
        maxwell_gmem_base=0x340000000ull;maxwell_gmem_size=0xd0000000ull;
        maxwell_gmem_map_all=maxwell_gmem_miss_log=maxwell_fp32_double=true;
        maxwell_brx_window=maxwell_static_stacks=maxwell_no_switch=maxwell_no_local_struct=maxwell_explicit_fallthrough=true;
        maxwell_native_half=maxwell_fast_minmax=true;
        auto start=std::chrono::steady_clock::now();
        shader_archive::Romfs rom(image.c_str());auto catalog=shader_catalog::read(rom,[](const std::string&){});
        auto selection=shader_warm::select(catalog,shader_warm::read(manifest.c_str()),title);
        std::set<uint64_t> keys;size_t bytes=0,hits=0,misses=0;
        auto stats=shader_warm::plan(catalog,selection,{8,false,true},[](const shader_archive::Shader& shader,const ShaderKey& key){
            return shader_warm::Info(maxwell_translate_bounded(shader.binary.data(),shader.constants,shader.binary.size()-shader.constants,shader.stage,&key),maxwell_free);
        },[&](unsigned,const ShaderKey&,const ShaderInfo& info){
            auto key=vk_shader_prepare_key(info.msl,info.stage);
            if(keys.insert(key).second) {
                bytes+=strlen(info.msl);
                if(keys_file.is_open())keys_file<<info.stage<<'\t'<<std::hex<<key<<std::dec<<'\n';
                if(!cache.empty()){if(vk_shader_cached(key))++hits;else ++misses;}
            }
        });
        auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"{\"requests\":"<<stats.requests<<",\"source_requests\":"<<stats.sources<<",\"unique_sources\":"<<keys.size()
                 <<",\"vertex\":"<<stats.stages[0]<<",\"fragment\":"<<stats.stages[1]<<",\"compute\":"<<stats.stages[5]
                 <<",\"unpaired_fragments\":"<<stats.unpaired_fragments<<",\"source_bytes\":"<<bytes<<",\"seconds\":"<<elapsed
                 <<",\"title_only\":"<<(title?"true":"false")<<",\"spirv_compiled\":false}\n";
        if(!cache.empty()) {
            auto counts=vk_shader_cache_stats();
            std::cout<<"{\"cache_namespace\":\""<<vk_shader_cache_namespace()<<"\",\"validated_hits\":"<<hits<<",\"misses\":"<<misses
                     <<",\"current_hits\":"<<counts.current_hits<<",\"compatible_hits\":"<<counts.compatible_hits<<"}\n";
        }
        if(keys_file.is_open()){keys_file.flush();if(!keys_file)throw std::runtime_error("cannot write key inventory");}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
