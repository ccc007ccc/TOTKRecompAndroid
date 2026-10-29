// Real compiler/session/cache integration; no GPU or gameplay execution.
#include "../runtime/nvn/vulkan/shader_prepare_queue.hpp"
#include <cstdarg>
#include <cstdio>
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
VkG g_vk;
void rt_fatal_trace(void){abort();}
#ifdef __ANDROID__
void rt_fatal_message(const char* format,...){va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);abort();}
#endif
}
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <unistd.h>
int main(int argc,char** argv) {
    assert(argc==3 || (argc==4 && !strcmp(argv[3],"--families")));auto input=std::filesystem::absolute(argv[1]);auto output=std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(output);assert(!chdir(output.c_str()));
    struct Source {std::string name,text;int stage;};std::vector<Source> sources;
    for(const auto& file:std::filesystem::directory_iterator(input)) {
        auto ext=file.path().extension();int stage=ext==".vert"?STAGE_VERTEX:ext==".frag"?STAGE_FRAGMENT:ext==".comp"?STAGE_COMPUTE:-1;
        if(stage<0)continue;std::ifstream f(file.path());assert(f);sources.push_back({file.path().filename(),{std::istreambuf_iterator<char>(f),{}},stage});
    }
    assert(!sources.empty());auto started=std::chrono::steady_clock::now();
    ShaderPrepareQueue queue(4);
    if(argc==4) {
        std::map<std::string,const Source*> names;for(const auto& s:sources)names.emplace(s.name,&s);
        std::ifstream f(input/"families.txt");assert(f);std::string base,name,kinds;
        std::map<std::string,std::vector<ShaderPrepareVariant>> families;std::unordered_set<std::string> listed;
        while(f>>base>>name>>kinds) {
            assert(names.count(base) && names.count(name) && kinds.size()==32 && listed.insert(name).second);
            const auto& s=*names.at(name);assert(names.at(base)->stage==s.stage);
            ShaderPrepareVariant v{};v.key=vk_shader_prepare_key(s.text.c_str(),s.stage);
            for(unsigned i=0;i<32;++i){assert(kinds[i]>='0' && kinds[i]<='2');v.kinds[i]=kinds[i]-'0';}
            families[base].push_back(v);
        }
        assert(f.eof() && listed.size()==sources.size());
        for(unsigned repeat=0;repeat<2;++repeat)for(const auto& family:families) {
            const auto& s=*names.at(family.first);
            // Match the preparation loop's bounded families; a dictionary can
            // be used by several workers when a family exceeds one batch.
            for(size_t first=0;first<family.second.size();first+=2048){
                auto end=std::min(first+2048,family.second.size());
                queue.submit(s.text.c_str(),s.stage,s.name,{family.second.begin()+first,family.second.begin()+end});
            }
        }
    } else {
        for(const auto& s:sources)queue.submit(s.text.c_str(),s.stage,s.name);
        for(const auto& s:sources)queue.submit(s.text.c_str(),s.stage,s.name);
    }
    queue.finish();assert(queue.done()==sources.size()*2);
    uint64_t raw_bytes=0,raw_unique=0,stored_bytes=0,base_bytes=0;std::unordered_set<uint64_t> keys;
    for(const auto& s:sources) {
        size_t words=0;auto binary=vk_load_prepared_shader(s.text.c_str(),s.stage,s.name.c_str(),&words);assert(binary && words>5);
        std::ofstream f(s.name+".spv",std::ios::binary);f.write((const char*)binary,words*4);assert(f);raw_bytes+=words*4;free(binary);
        if(keys.insert(vk_shader_prepare_key(s.text.c_str(),s.stage)).second)raw_unique+=words*4;
    }
    char why[512];assert(!vk_shader_prepare_source(sources[0].text.c_str(),sources[0].stage,"outside-session",why,sizeof why));
    assert(!vk_shader_prepare_variant(nullptr,nullptr,why,sizeof why));
    for(const auto& file:std::filesystem::recursive_directory_iterator("cache"))if(file.is_regular_file()) {
        stored_bytes+=file.file_size();if(file.path().stem().extension()==".base")base_bytes+=file.file_size();
    }
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    std::ofstream report("report.json");report<<"{\"sources\":"<<sources.size()<<",\"unique_sources\":"<<queue.unique_sources()<<",\"shared_bases\":"<<queue.unique_bases()<<",\"raw_output_bytes_including_duplicates\":"<<raw_bytes
        <<",\"raw_unique_bytes\":"<<raw_unique<<",\"stored_cache_bytes\":"<<stored_bytes<<",\"base_cache_bytes\":"<<base_bytes<<",\"seconds\":"<<seconds<<"}\n";
    std::cout<<"PASS: "<<sources.size()<<" shaders prepared with four compiler sessions; compressed cache loads and closed-session rejection passed\n";
}
