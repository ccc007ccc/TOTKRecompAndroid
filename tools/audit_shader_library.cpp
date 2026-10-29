// Whole-RomFS translator audit. Build with the production translator, on Linux or Android.
// Passing this audit proves translation coverage only, not SPIR-V/device correctness.
#include "../runtime/nvn/vulkan/shader_archive.hpp"
#include <array>
#include <iomanip>
#include <iostream>
#include <dlfcn.h>
#include "../runtime/nvn/vulkan/shader_prepare.h"
extern "C" {
#include "../runtime/nvn/maxwell/shader.h"
}
static void json_string(const char *s) {
    std::cout << '"';
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
        if (*p=='"' || *p=='\\') std::cout << '\\' << char(*p);
        else if (*p<32) std::cout << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(*p) << std::dec;
        else std::cout << char(*p);
    }
    std::cout << '"';
}
int main(int argc, char **argv) {
    const char *compiler_path=nullptr;
    bool fp32=false, valid=argc>=2;
    for (int i=2; i<argc && valid; ++i) {
        if (!strcmp(argv[i],"--fp32-double")) fp32=true;
        else if (!strcmp(argv[i],"--compile") && i+1<argc) compiler_path=argv[++i];
        else valid=false;
    }
    if (!valid) {
        std::cerr << "usage: audit_shader_library ROMFS_IMAGE [--fp32-double] [--compile LIBSHADERPREPARE]\n"; return 2;
    }
    try {
        void *compiler=nullptr; ShaderPrepareCompile compile=nullptr;
        if (compiler_path) {
            compiler=dlopen(compiler_path,RTLD_NOW|RTLD_LOCAL);
            if(compiler)compile=(ShaderPrepareCompile)dlsym(compiler,"shader_prepare_compile");
            if(!compile)throw std::runtime_error("cannot load preparation compiler");
        }
        shader_archive::Romfs rom(argv[1]);
        maxwell_gmem_base=0x340000000ull;maxwell_gmem_size=0xd0000000ull;
        maxwell_no_int64=maxwell_gmem_map_all=maxwell_brx_window=maxwell_static_stacks=true;
        maxwell_structured=maxwell_native_half=maxwell_fast_minmax=true;
        maxwell_fp32_double=fp32;
        std::set<shader_archive::Bytes> seen;
        std::array<unsigned,6> count{};unsigned failed=0,translation_failed=0,validated=0;
        std::cout << "{\"scope\":\"" << (compile?"library_spirv_validation":"translation_only")
                  << "\",\"double_arithmetic\":\"" << (fp32?"fp32-preferred":"integer-binary64") << "\",\"failures\":[";
        shader_archive::inventory(rom,[&](shader_archive::Shader s){
            auto key=shader_archive::input_identity(s);
            if(!seen.insert(std::move(key)).second)return;
            count[s.stage]++;
            ShaderKey k{};k.glsl=1;k.vs_out_mask=~0u;k.gather_bias_bits=8;
            ShaderInfo *info=maxwell_translate_bounded(s.binary.data(),s.constants,s.binary.size()-s.constants,s.stage,&k);
            char error[8192]{}; const char *reason=nullptr;
            if(!info || !info->msl) { ++translation_failed; reason=info?info->error:"allocation failure"; }
            else if (compile) {
                size_t words=0;
                auto binary=compile(info->msl,s.stage,0x00401000,false,&words,error,sizeof error);
                if(binary){++validated;free(binary);}else reason=error;
            }
            if(reason) {
                uint64_t h=0xcbf29ce484222325ull;for(auto b:s.binary)h=(h^b)*0x100000001b3ull;
                if(failed++)std::cout<<',';
                std::cout << "{\"stage\":"<<s.stage<<",\"code\":\""<<std::hex<<h<<std::dec<<"\",\"reason\":";
                json_string(reason);std::cout<<'}';
            }
            maxwell_free(info);
            if(compile && seen.size()%64==0)std::cerr<<"audited "<<seen.size()<<", validated "<<validated<<", failed "<<failed<<'\n';
        },[](const std::string&){});
        std::cout<<"],\"total\":"<<seen.size()<<",\"vertex\":"<<count[0]<<",\"fragment\":"<<count[1]
            <<",\"compute\":"<<count[5]<<",\"failed\":"<<failed<<",\"translation_failed\":"<<translation_failed
            <<",\"spirv_validated\":"<<validated<<",\"spirv_compiled\":"<<(compile?"true":"false")<<"}\n";
        if(compiler)dlclose(compiler);
        return failed?1:0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 2;}
}
