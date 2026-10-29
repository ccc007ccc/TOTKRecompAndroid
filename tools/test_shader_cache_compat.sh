#!/usr/bin/env bash
# Run in dev. Optional OLD_PLUGIN and GLSL paths compare compiler output bytes.
set -euo pipefail
cd "$(dirname "$0")/.."
SHADER_COMPAT_OUT=${1:-.work/shader-cache-compat}
mkdir -p "$SHADER_COMPAT_OUT"
SHADER_COMPAT_OUT=$(cd "$SHADER_COMPAT_OUT" && pwd)
python tools/test_shader_cache_contract.py
sh tools/build_host_shader_compiler.sh "$SHADER_COMPAT_OUT"
clang -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iandroid/deps/out/include \
    tools/test_shader_cache_compat.c -Wl,--gc-sections -lzstd -ldl -pthread -o "$SHADER_COMPAT_OUT/test-compat"
LARECOMP_NATIVE_LIB_DIR="$SHADER_COMPAT_OUT" "$SHADER_COMPAT_OUT/test-compat" "$SHADER_COMPAT_OUT"
if [[ $# -ge 2 ]]; then
    SHADER_COMPAT_OLD=$2
    shift 2
    clang -O2 tools/test_shader_compiler_compat.c -ldl -o "$SHADER_COMPAT_OUT/test-compiler"
    "$SHADER_COMPAT_OUT/test-compiler" "$SHADER_COMPAT_OLD" "$SHADER_COMPAT_OUT/libshaderprepare.so" "$@"
fi
