// Optional private RomFS corpus: validate every surface program through the
// production compiler and reflection, across scalar types and memory profiles.
#include "../runtime/nvn/vulkan/shader_catalog.hpp"
#include "../runtime/nvn/vulkan/shader_prepare.h"
#include <dlfcn.h>
#include <fstream>
#include <filesystem>
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
}
int main(int argc,char**argv) {
    if(argc!=4){fprintf(stderr,"usage: test_shader_images ROMFS COMPILER OUTPUT_DIRECTORY\n");return 2;}
    void *library=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!library){fprintf(stderr,"%s\n",dlerror());return 2;}
    auto compile=(ShaderPrepareCompile)dlsym(library,"shader_prepare_compile");if(!compile)return 2;
    std::filesystem::create_directories(argv[3]);shader_archive::Romfs rom(argv[1]);auto catalog=shader_catalog::read(rom,[](auto){});
    maxwell_gmem_base=0x340000000ull;maxwell_gmem_size=0xd0000000ull;
    maxwell_brx_window=maxwell_static_stacks=maxwell_structured=maxwell_native_half=maxwell_fast_minmax=maxwell_fp32_double=true;
    unsigned selected=0,checked=0;
    for(unsigned id=0;id<catalog.records.size();++id){const auto&s=catalog.records[id].shader;if(s.stage!=STAGE_COMPUTE)continue;
        ShaderKey key{};key.glsl=1;maxwell_gmem_map_all=maxwell_no_int64=true;
        auto initial=maxwell_translate_bounded(s.binary.data(),s.constants,s.binary.size()-s.constants,s.stage,&key);
        if(!initial || !initial->msl){fprintf(stderr,"translation %u: %s\n",id,initial?initial->error:"allocation");return 1;}
        bool image=false;for(int i=0;i<initial->ntex;++i)image|=initial->tex[i].is_image;
        bool formatted=initial->image_format_mask;maxwell_free(initial);if(!image)continue;++selected;
        for(unsigned profile=0;profile<4;++profile)for(unsigned kind=0;kind<(formatted?3u:1u);++kind){
            maxwell_gmem_map_all=profile&1;maxwell_no_int64=profile&2;
            for(auto& k:key.image_kind)k=kind;
            auto info=maxwell_translate_bounded(s.binary.data(),s.constants,s.binary.size()-s.constants,s.stage,&key);
            if(!info || !info->msl){fprintf(stderr,"translation %u/%u/%u: %s\n",id,profile,kind,info?info->error:"allocation");return 1;}
            std::string name=std::string(argv[3])+"/image-"+std::to_string(id)+"-"+std::to_string(profile)+"-"+std::to_string(kind);
            std::ofstream(name+".comp")<<info->msl;
            char why[8192]{};size_t count=0;auto binary=compile(info->msl,STAGE_COMPUTE,VK_API_VERSION_1_1,false,&count,why,sizeof why);
            if(!binary){fprintf(stderr,"compile %s: %s\n",name.c_str(),why);return 1;}
            VkBindingInfo bindings[96];int n=vk_reflect(binary,count,bindings,96),images=0;
            for(int i=0;i<n;++i)if(bindings[i].binding>=32){
                unsigned index=bindings[i].binding-32;
                if(index>=(unsigned)info->ntex || bindings[i].type!=(info->tex[index].is_image?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)){
                    fprintf(stderr,"wrong reflection %s binding %u type %u\n",name.c_str(),bindings[i].binding,bindings[i].type);return 1;
                }
                images+=info->tex[index].is_image;
            }
            if(!images){fprintf(stderr,"no storage descriptors in %s\n",name.c_str());return 1;}
            std::ofstream f(name+".spv",std::ios::binary);f.write((const char*)binary,count*4);if(!f)return 2;
            free(binary);maxwell_free(info);++checked;
        }
        printf("validated surface program %u (%u variants so far)\n",id,checked);fflush(stdout);
    }
    printf("PASS: %u surface programs, %u SPIR-V/reflection variants\n",selected,checked);dlclose(library);return selected?0:1;
}
