// Explicit preparation and enabled runtime cache misses load this plugin.
#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>
#include <stdio.h>
#include "shader_prepare.h"
#include "shader_precision.h"
#include "../maxwell/shader.h"

#ifdef _WIN32
#define PREP_EXPORT __declspec(dllexport)
#else
#define PREP_EXPORT __attribute__((visibility("default")))
#endif
PREP_EXPORT void *shader_prepare_create(void) { return shaderc_compiler_initialize(); }
PREP_EXPORT void shader_prepare_destroy(void *compiler) { if (compiler) shaderc_compiler_release(compiler); }
PREP_EXPORT uint32_t *shader_prepare_compile_context(void *context, const char *src, int stage, unsigned api, bool optimized,
                                size_t *nwords, char *why, size_t capacity) {
    *nwords = 0;
    if (stage != STAGE_VERTEX && stage != STAGE_FRAGMENT && stage != STAGE_COMPUTE) {
        snprintf(why, capacity, "unsupported shader stage %d", stage); return NULL;
    }
    shaderc_compiler_t compiler = context;
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    if (!compiler || !options) {
        if (options) shaderc_compile_options_release(options);
        snprintf(why, capacity, "cannot allocate shader compiler"); return NULL;
    }
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
        api >= ((1u << 22) | (3u << 12)) ? shaderc_env_version_vulkan_1_3 :
        api >= ((1u << 22) | (2u << 12)) ? shaderc_env_version_vulkan_1_2 : shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(options, optimized ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);
    shaderc_shader_kind kind = stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
    shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, src, strlen(src), kind, "prepare", "main", options);
    uint32_t *out = NULL;
    if (!result || shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
        snprintf(why, capacity, "%s", result ? shaderc_result_get_error_message(result) : "shader compiler returned no result");
    else {
        size_t bytes = shaderc_result_get_length(result);
        out = malloc(bytes);
        if (out) {
            memcpy(out, shaderc_result_get_bytes(result), bytes); *nwords = bytes / 4;
            if (stage == STAGE_VERTEX || (stage == STAGE_FRAGMENT && writes_fragment_depth(out, *nwords)))
                out = no_contraction(out, nwords);
            if (!out) snprintf(why, capacity, "out of memory preserving shader depth precision");
        } else snprintf(why, capacity, "out of memory compiling shader");
    }
    if (result) shaderc_result_release(result);
    shaderc_compile_options_release(options);
    if (out) {
        spv_target_env env = api >= ((1u << 22) | (3u << 12)) ? SPV_ENV_VULKAN_1_3 :
                            api >= ((1u << 22) | (2u << 12)) ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_1;
        spv_context context = spvContextCreate(env);
        spv_validator_options validation = spvValidatorOptionsCreate();
        // Only diagnostic spelling changes. All validation rules keep their
        // defaults; eager friendly-name generation is expensive on large modules.
        if (validation) spvValidatorOptionsSetFriendlyNames(validation, false);
        spv_const_binary_t binary = {out, *nwords};
        spv_diagnostic diagnostic = NULL;
        if (!context || !validation || spvValidateWithOptions(context, validation, &binary, &diagnostic) != SPV_SUCCESS) {
            snprintf(why, capacity, "SPIR-V validation: %s", diagnostic ? diagnostic->error : "cannot allocate validator");
            free(out); out = NULL; *nwords = 0;
        }
        spvDiagnosticDestroy(diagnostic);
        spvValidatorOptionsDestroy(validation);
        spvContextDestroy(context);
    }
    return out;
}
// Standalone audit ABI; production preparation retains one compiler per worker.
PREP_EXPORT uint32_t *shader_prepare_compile(const char *src, int stage, unsigned api, bool optimized,
                                            size_t *nwords, char *why, size_t capacity) {
    void *compiler=shader_prepare_create();
    uint32_t *out=shader_prepare_compile_context(compiler,src,stage,api,optimized,nwords,why,capacity);
    shader_prepare_destroy(compiler);return out;
}
