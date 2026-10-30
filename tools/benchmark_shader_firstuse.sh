#!/usr/bin/env bash
# Run in dev after android/dev_env.sh: ROMFS OUTPUT_DIRECTORY.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# != 2 ]]; then
    echo 'usage: benchmark_shader_firstuse.sh ROMFS_IMAGE OUTPUT_DIRECTORY' >&2
    exit 2
fi
image=$(realpath "$1")
out=$(realpath -m "$2")
unset LARECOMP_VK_OPT LARECOMP_NO_BANK2 LARECOMP_NO_DISCARD LARECOMP_SAVE_MSL
unset LARECOMP_SHADER_ARTIFACTS LARECOMP_SHADER_SUBST LARECOMP_DUMP_VERTEX_INPUT LARECOMP_DUMP_VERTEX_OUTPUT LARECOMP_DIAGNOSTIC_VS LARECOMP_DUMP_FS
mkdir -p "$out/cache-probe"
sh tools/build_host_shader_compiler.sh "$out/plugin"
flags=(-O2 -ffunction-sections -fdata-sections -Iandroid/deps/out/include)
${CC:-clang} "${flags[@]}" -c runtime/nvn/maxwell/translate.c -o "$out/translate.o"
${CC:-clang} "${flags[@]}" -c runtime/nvn/vulkan/shaders.c -o "$out/shaders.o"
${CC:-clang} "${flags[@]}" -c runtime/nvn/vulkan/compile_budget.c -o "$out/compile_budget.o"
${CC:-clang} "${flags[@]}" -c runtime/nvn/vulkan/shader_keys.c -o "$out/shader_keys.o"
${CC:-clang} "${flags[@]}" -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o "$out/digest.o"
${CXX:-clang++} -std=c++17 "${flags[@]}" -Ithird_party/d2r-digest/include \
    tools/benchmark_shader_firstuse.cpp runtime/nvn/vulkan/shader_artifact.cpp runtime/nvn/vulkan/shader_artifact_storage.cpp \
    "$out/translate.o" "$out/shaders.o" "$out/compile_budget.o" "$out/shader_keys.o" "$out/digest.o" \
    -Wl,--gc-sections -lzstd -ldl -pthread -o "$out/benchmark"
LARECOMP_NATIVE_LIB_DIR="$out/plugin" "$out/benchmark" "$image" "$out/cache-probe"
