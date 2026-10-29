#!/usr/bin/env bash
# Run inside dev: bash tools/test_shader_dictionaries.sh [OUTPUT_DIRECTORY] [ROMFS]
set -euo pipefail
cd "$(dirname "$0")/.."
SHADER_TEST_OUT=${1:-.work/totk-port/shader-dictionaries}
mkdir -p "$SHADER_TEST_OUT"
SHADER_TEST_OUT=$(cd "$SHADER_TEST_OUT" && pwd)
CC=${CC:-clang}
CXX=${CXX:-clang++}
FLAGS=(-O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iandroid/deps/out/include)
LIBS=(-Wl,--gc-sections -lzstd -ldl -pthread)
export LARECOMP_NATIVE_LIB_DIR="$SHADER_TEST_OUT"
unset LARECOMP_VK_OPT

CC="$CC" CXX="$CXX" sh tools/build_host_shader_compiler.sh "$SHADER_TEST_OUT"
$CC "${FLAGS[@]}" -c runtime/nvn/vulkan/shaders.c -o "$SHADER_TEST_OUT/shaders.o"
$CC -O2 -c runtime/nvn/maxwell/translate.c -o "$SHADER_TEST_OUT/translate.o"
$CXX -std=c++17 "${FLAGS[@]}" tools/test_shader_prepare_workers.cpp "$SHADER_TEST_OUT/shaders.o" \
    "${LIBS[@]}" -o "$SHADER_TEST_OUT/test-workers"
$CXX -std=c++17 "${FLAGS[@]}" tools/test_shader_prepare_ready.cpp "$SHADER_TEST_OUT/shaders.o" \
    "$SHADER_TEST_OUT/translate.o" "${LIBS[@]}" -o "$SHADER_TEST_OUT/test-ready"
"$SHADER_TEST_OUT/test-ready" "$SHADER_TEST_OUT/ready-tests"
for test in queue variant_plan; do
    source=tools/test_shader_prepare_queue.cpp
    if [[ $test == variant_plan ]]; then source=tools/test_shader_variant_plan.cpp; fi
    $CXX -std=c++17 "${FLAGS[@]}" "$source" "${LIBS[@]}" -o "$SHADER_TEST_OUT/test-$test"
    "$SHADER_TEST_OUT/test-$test"
done
for test in dictionary depth_precision; do
    mkdir -p "$SHADER_TEST_OUT/$test-tests"
    $CC "${FLAGS[@]}" "tools/test_shader_$test.c" "${LIBS[@]}" -o "$SHADER_TEST_OUT/test-$test"
    "$SHADER_TEST_OUT/test-$test" "$SHADER_TEST_OUT/$test-tests"
done
# Match the Android build's SMAA literal generation without requiring Gradle.
python3 - "$SHADER_TEST_OUT/smaa_hlsl.inc" <<'PY'
from pathlib import Path
import sys
raw = Path('third_party/smaa/SMAA.hlsl').read_bytes().replace(b'\r', b'')
lines = bytes(c if c < 128 else ord('?') for c in raw).decode('ascii').splitlines()
Path(sys.argv[1]).write_text(''.join('"' + s.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n' for s in lines))
PY
mkdir -p "$SHADER_TEST_OUT/builtin-tests"
$CC "${FLAGS[@]}" -Ithird_party/smaa/Textures -I"$SHADER_TEST_OUT" tools/test_shader_preparation.c \
    runtime/nvn/smaa_data.c "${LIBS[@]}" -o "$SHADER_TEST_OUT/test-preparation"
"$SHADER_TEST_OUT/test-preparation" "$SHADER_TEST_OUT/builtin-tests"
SAMPLES=()
if [[ $# -ge 2 ]]; then
    $CXX -std=c++17 -O2 tools/audit_shader_variants.cpp "$SHADER_TEST_OUT/translate.o" -lzstd -o "$SHADER_TEST_OUT/audit-variants"
    "$SHADER_TEST_OUT/audit-variants" "$2" "$SHADER_TEST_OUT/plan" --samples
    SAMPLES=("$SHADER_TEST_OUT/plan")
fi
python3 tools/test_shader_dictionaries.py "$SHADER_TEST_OUT/test-workers" "$SHADER_TEST_OUT/integration" "${SAMPLES[@]}"
