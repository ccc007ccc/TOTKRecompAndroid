// Bounds/unsupported-input regression for the preparation-only SPIR-V parser.
#include "../runtime/nvn/vulkan/shader_interface.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <spirv-tools/libspirv.h>

int main(int argc,char** argv) {
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);assert(f);
    std::string bytes{std::istreambuf_iterator<char>(f),{}};assert(bytes.size()%4==0);
    shader_interface::Words base(bytes.size()/4);memcpy(base.data(),bytes.data(),bytes.size());
    uint8_t kinds[32]{};kinds[0]=1;kinds[1]=2;
    auto result=shader_interface::specialize(base.data(),base.size(),0,kinds);assert(result.size()>base.size());
    unsigned attempts=0,rejected=0;
    auto context=spvContextCreate(SPV_ENV_VULKAN_1_1);assert(context);
    auto attempt=[&](const shader_interface::Words& words,int stage=0){
        ++attempts;try{auto out=shader_interface::specialize(words.data(),words.size(),stage,kinds);
            if(spvValidateBinary(context,out.data(),out.size(),nullptr)!=SPV_SUCCESS)++rejected;}
        catch(const std::exception&){++rejected;}
    };
    auto n=rejected;attempt(base,1);assert(rejected==n+1); // caller stage must match entry point
    for(size_t length=0;length<base.size();++length)attempt({base.begin(),base.begin()+length});
    for(size_t word=0;word<base.size();++word)for(auto replacement:{0u,1u,0xffffffffu,base[3]}) {
        auto changed=base;changed[word]=replacement;attempt(changed);
    }
    kinds[0]=3;n=rejected;attempt(base);assert(rejected==n+1);
    spvContextDestroy(context);
    std::cout<<"PASS: "<<attempts<<" malformed/stage/type cases, "<<rejected<<" rejected; parser bounds intact\n";
}
