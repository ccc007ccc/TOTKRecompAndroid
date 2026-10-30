#!/bin/sh
# GPU test of the Vulkan shader path (tools/test_vk_pipelines.c): fuzzed Maxwell programs -> GLSL -> the
# backend's compile/reflection/layouts -> real pipelines on the device, under the validation layer if present.
# usage: tools/test_vk_pipelines.sh [count] [seed] (dev container / MSYS2 ucrt64)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_vk_pipelines
mkdir -p "$OUT"
CC=${CC:-clang}
CXX=${CXX:-clang++}
case "$(uname -s)" in Linux*) VK=-lvulkan; DL=-ldl ;; *) VK=-lvulkan-1; DL= ;; esac
CC="$CC" CXX="$CXX" sh tools/build_host_shader_compiler.sh "$OUT"
$CC -O2 -w -ffunction-sections -fdata-sections -Iruntime -Iruntime/nvn -Iandroid/deps/out/include \
    -D_FILE_OFFSET_BITS=64 tools/test_vk_pipelines.c runtime/nvn/maxwell/translate.c \
    runtime/nvn/vulkan/shaders.c runtime/nvn/vulkan/compile_budget.c -Wl,--gc-sections -o "$OUT/test_vk_pipelines" $VK $DL -lzstd -lshaderc_shared -lSPIRV-Tools-shared
export LARECOMP_NATIVE_LIB_DIR="$PWD/$OUT"
if command -v cygpath >/dev/null; then export LARECOMP_NATIVE_LIB_DIR="$(cygpath -m "$LARECOMP_NATIVE_LIB_DIR")"; fi
# the MSYS2 validation layer manifest lives next to its DLL
for d in /ucrt64/bin /c/msys64/ucrt64/bin; do
  [ -f "$d/VkLayer_khronos_validation.json" ] && export VK_ADD_LAYER_PATH="$(cygpath -w "$d")"
done
"$OUT/test_vk_pipelines" runtime/nvn/maxwell/translate.c "${1:-2000}" "${2:-1}"
