# Content identities have separate contracts: generated source/interface,
# preparation coverage, and Vulkan recipe ABI/builder. Throughput and unrelated
# command recording changes do not invalidate exact shader mappings.
function(totk_shader_identity root)
    set(translation
        runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h runtime/nvn/maxwell/soft64.inc
        runtime/nvn/vulkan/shader_config.c runtime/nvn/vulkan/shader_config.h
        runtime/nvn/vulkan/shader_keys.c runtime/nvn/vulkan/shader_keys.h
        runtime/nvn/vulkan/shader_source_key.h
        runtime/nvn/vulkan/shader_artifact.cpp runtime/nvn/vulkan/shader_artifact.h
        runtime/nvn/vulkan/shader_spirv.h runtime/nvn/vertex_format.h runtime/rt.h)
    set(preparation
        runtime/nvn/vulkan/shader_prepare.cpp runtime/nvn/vulkan/shader_prepare.h
        runtime/nvn/vulkan/shader_archive.hpp runtime/nvn/vulkan/shader_catalog.hpp
        runtime/nvn/vulkan/shader_material.hpp runtime/nvn/vulkan/shader_variant_plan.hpp
        runtime/nvn/vulkan/shader_cache_receipt.hpp runtime/nvn/vulkan/shader_warm_state.hpp)
    set(plan
        runtime/nvn/vulkan/shader_warm_plan.hpp runtime/nvn/vulkan/shader_warm_manifest.hpp
        runtime/nvn/vulkan/shader_warm_requests.hpp tools/build_shader_warm_manifest.py)
    set(recipe
        runtime/nvn/vulkan/pipeline_state.h runtime/nvn/vulkan/pipeline_state.c
        runtime/nvn/vulkan/pipeline_seed.h runtime/nvn/vulkan/pipeline_seed.c
        runtime/nvn/vulkan/pipeline_warm.h)
    foreach(contract translation preparation plan recipe)
        set(input "totk-${contract}-v1;")
        foreach(source IN LISTS ${contract})
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${source}")
            file(SHA256 "${root}/${source}" digest)
            string(APPEND input "${source}:${digest};")
        endforeach()
        string(SHA256 ${contract}_id "${input}")
    endforeach()
    string(SHA256 prepare_id "${translation_id}:${preparation_id}:${plan_id}")
    set(SHADER_TRANSLATION_ID "${translation_id}" PARENT_SCOPE)
    set(SHADER_PREPARE_ID "${prepare_id}" PARENT_SCOPE)
    set(SHADER_PLAN_ID "${plan_id}" PARENT_SCOPE)
    set(PIPELINE_RECIPE_ID "${recipe_id}" PARENT_SCOPE)
endfunction()
