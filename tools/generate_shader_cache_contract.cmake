# Standalone/native checks use the same contract as the Android application.
cmake_minimum_required(VERSION 3.22)
if(NOT SHADER_CACHE_ROOT OR NOT SHADER_CACHE_HEADER)
    message(FATAL_ERROR "Set SHADER_CACHE_ROOT and SHADER_CACHE_HEADER")
endif()
include("${SHADER_CACHE_ROOT}/runtime/nvn/vulkan/shader_cache_contract.cmake")
totk_generate_shader_cache_contract("${SHADER_CACHE_ROOT}" "${SHADER_CACHE_HEADER}")
