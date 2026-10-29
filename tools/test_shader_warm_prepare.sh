#!/usr/bin/env bash
# Run inside dev: bash tools/test_shader_warm_prepare.sh [OUTPUT] [ROMFS]
set -euo pipefail
cd "$(dirname "$0")/.."
WARM_PREP_OUT=${1:-.work/shader-warm-prepare-tests}
mkdir -p "$WARM_PREP_OUT"
WARM_PREP_OUT=$(cd "$WARM_PREP_OUT" && pwd)
FLAGS=(-O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iandroid/deps/out/include)
sh tools/build_host_shader_compiler.sh "$WARM_PREP_OUT"
clang "${FLAGS[@]}" -c runtime/nvn/vulkan/shaders.c -o "$WARM_PREP_OUT/shaders.o"
clang "${FLAGS[@]}" -c runtime/nvn/maxwell/translate.c -o "$WARM_PREP_OUT/translate.o"
clang "${FLAGS[@]}" -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o "$WARM_PREP_OUT/digest.o"
clang++ -std=c++17 "${FLAGS[@]}" tools/test_shader_warm_plan.cpp "$WARM_PREP_OUT/translate.o" "$WARM_PREP_OUT/digest.o" \
    -Wl,--gc-sections -lzstd -o "$WARM_PREP_OUT/test-plan"
"$WARM_PREP_OUT/test-plan"
clang++ -std=c++17 "${FLAGS[@]}" tools/test_shader_warm_receipt.cpp -Wl,--gc-sections -o "$WARM_PREP_OUT/test-receipt"
"$WARM_PREP_OUT/test-receipt" "$WARM_PREP_OUT/receipt-unit"
if [[ $# -ge 2 ]]; then
    python tools/build_shader_warm_manifest.py "$WARM_PREP_OUT/manifest.bin"
    clang++ -std=c++17 "${FLAGS[@]}" tools/test_shader_warm_prepare.cpp runtime/nvn/vulkan/shader_warm.cpp \
        "$WARM_PREP_OUT/shaders.o" "$WARM_PREP_OUT/translate.o" "$WARM_PREP_OUT/digest.o" \
        -Wl,--gc-sections -lzstd -ldl -pthread -o "$WARM_PREP_OUT/test-prepare"
    export LARECOMP_NATIVE_LIB_DIR="$WARM_PREP_OUT"
    unset LARECOMP_VK_OPT
    extra=();if [[ $# -ge 3 ]]; then extra+=("$3");fi
    "$WARM_PREP_OUT/test-prepare" "$2" "$WARM_PREP_OUT/manifest.bin" "$WARM_PREP_OUT/result" "${extra[@]}"
fi
