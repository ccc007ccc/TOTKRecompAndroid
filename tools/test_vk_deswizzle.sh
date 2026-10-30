#!/bin/sh
# Run inside dev. No game data; requires Vulkan 1.3 and the validation layer.
set -eu
cd "$(dirname "$0")/.."
out=${1:-build/test_vk_deswizzle}
mkdir -p "$out"
cc=${CC:-clang}
$cc -O1 -g -fsanitize=address,undefined tools/test_deswizzle_params.c runtime/nvn/tegra.c -o "$out/params"
"$out/params" "$out/deswizzle.comp"
glslc --target-env=vulkan1.1 "$out/deswizzle.comp" -o "$out/deswizzle.comp.spv"
spirv-val --target-env vulkan1.1 "$out/deswizzle.comp.spv"
$cc -O2 -g -ffunction-sections -fdata-sections -Iandroid/deps/out/include \
    tools/test_vk_deswizzle.c runtime/nvn/vulkan/deswizzle.c runtime/nvn/tegra.c \
    -Wl,--gc-sections,--wrap=vkCreateDescriptorSetLayout,--wrap=vkCreatePipelineLayout,--wrap=vkCreateComputePipelines \
    -Wl,--wrap=vkDestroyDescriptorSetLayout,--wrap=vkDestroyPipelineLayout,--wrap=vkDestroyPipeline \
    -lSDL3 -lvulkan -o "$out/gpu"
"$out/gpu" "$out/deswizzle.comp.spv"
