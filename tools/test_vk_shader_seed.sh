#!/usr/bin/env bash
# Run in dev: prepared warm output (contains cache/), then its seed path.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${3:-.work/vulkan-shader-seed-tests}
mkdir -p "$out"
clang -O2 -g -ffunction-sections -fdata-sections -Iandroid/deps/out/include -Ithird_party/d2r-digest/include \
    tools/test_vk_shader_seed.c runtime/nvn/vulkan/shaders.c runtime/nvn/vulkan/shader_module_cache.c \
    runtime/nvn/vulkan/pipeline_state.c runtime/nvn/vulkan/pipeline_warm.c runtime/nvn/vulkan/pipeline_seed.c \
    runtime/nvn/vulkan/compile_budget.c third_party/d2r-digest/digest.c \
    -Wl,--gc-sections -lvulkan -lzstd -ldl -pthread -o "$out/test-real-seed"
"$out/test-real-seed" "$(realpath "$1")" "$(realpath "$2")"
