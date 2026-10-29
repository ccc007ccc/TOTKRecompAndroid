#include "shader_interface.hpp"
#include <spirv-tools/libspirv.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define PREP_EXPORT __declspec(dllexport)
#else
#define PREP_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PREP_EXPORT uint32_t *shader_prepare_derive(
    const uint32_t* base,size_t words,int stage,const uint8_t* kinds,unsigned api,size_t* out_words,char* why,size_t capacity) {
    *out_words=0;
    try {
        auto result=shader_interface::specialize(base,words,stage,kinds);
        auto env=api>=((1u<<22)|(3u<<12))?SPV_ENV_VULKAN_1_3:api>=((1u<<22)|(2u<<12))?SPV_ENV_VULKAN_1_2:SPV_ENV_VULKAN_1_1;
        auto context=spvContextCreate(env);spv_diagnostic diagnostic=nullptr;
        auto options=spvValidatorOptionsCreate();
        // Keep full validation; numeric error IDs avoid rebuilding friendly names
        // for every interface variant of the same large module.
        if(options)spvValidatorOptionsSetFriendlyNames(options,false);
        spv_const_binary_t binary{result.data(),result.size()};
        auto status=context&&options?spvValidateWithOptions(context,options,&binary,&diagnostic):SPV_ERROR_OUT_OF_MEMORY;
        if(status!=SPV_SUCCESS)snprintf(why,capacity,"interface validation: %s",diagnostic?diagnostic->error:"cannot allocate validator");
        spvDiagnosticDestroy(diagnostic);spvValidatorOptionsDestroy(options);spvContextDestroy(context);if(status!=SPV_SUCCESS)return nullptr;
        auto out=static_cast<uint32_t*>(malloc(result.size()*4));if(!out)throw std::bad_alloc();
        memcpy(out,result.data(),result.size()*4);*out_words=result.size();return out;
    }catch(const std::exception& e){snprintf(why,capacity,"interface specialization: %s",e.what());return nullptr;}
}
