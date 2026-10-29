#!/bin/sh
# Host Vulkan execution test; run through distrobox enter dev.
set -eu
cd "$(dirname "$0")/.."
out=${1:-build/test_vk_surface}
mkdir -p "$out"
cc=${CC:-clang}
cxx=${CXX:-clang++}
$cc -O1 -g -fsanitize=address,undefined tools/test_shader_surface.c -lm -o "$out/generate"
"$out/generate" "$out"
for source in "$out"/surface-*.comp; do
    glslc --target-env=vulkan1.1 "$source" -o "$source.spv"
    spirv-val --target-env vulkan1.1 "$source.spv"
done
$cxx -O2 -w -ffunction-sections -fdata-sections -Iandroid/deps/out/include -c runtime/nvn/vulkan/vma.cpp -o "$out/vma.o"
$cc -O2 -ffunction-sections -fdata-sections -Iandroid/deps/out/include tools/test_vk_surface.c runtime/nvn/vulkan/textures.c \
    runtime/nvn/tegra.c runtime/nvn/formats.c "$out/vma.o" -Wl,--gc-sections -lSDL3 -lvulkan -lstdc++ -lm -o "$out/test-gpu"
"$out/test-gpu" "$out"
