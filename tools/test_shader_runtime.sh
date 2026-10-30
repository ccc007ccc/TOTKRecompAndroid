#!/usr/bin/env bash
# Run inside dev: bash tools/test_shader_runtime.sh [OUTPUT_DIRECTORY]
set -euo pipefail
cd "$(dirname "$0")/.."
SHADER_RUNTIME_OUT=${1:-.work/totk-port/shader-runtime-tests}
mkdir -p "$SHADER_RUNTIME_OUT"
SHADER_RUNTIME_OUT=$(cd "$SHADER_RUNTIME_OUT" && pwd)
CC=${CC:-clang}
CXX=${CXX:-clang++}
CC="$CC" CXX="$CXX" sh tools/build_host_shader_compiler.sh "$SHADER_RUNTIME_OUT"
$CC -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iandroid/deps/out/include \
    tools/test_shader_runtime.c runtime/nvn/vulkan/compile_budget.c -Wl,--gc-sections -lzstd -ldl -pthread -o "$SHADER_RUNTIME_OUT/test-runtime"
export LARECOMP_NATIVE_LIB_DIR="$SHADER_RUNTIME_OUT"
unset LARECOMP_VK_OPT
"$SHADER_RUNTIME_OUT/test-runtime" "$SHADER_RUNTIME_OUT"
