#!/usr/bin/env bash
# Run inside dev; no phone. All outputs are isolated.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-.work/totk-port/shader-artifact-tests}
mkdir -p "$out"
out=$(cd "$out" && pwd)
sanitizer=address,undefined
if [[ ${TOTK_ARTIFACT_TSAN:-0} == 1 ]]; then sanitizer=thread; fi
flags=(-O1 -g -fsanitize="$sanitizer" -ffunction-sections -fdata-sections -Iandroid/deps/out/include)
sh tools/build_host_shader_compiler.sh "$out"
clang "${flags[@]}" -c runtime/nvn/vulkan/shaders.c -o "$out/shaders.o"
clang "${flags[@]}" -c runtime/nvn/vulkan/compile_budget.c -o "$out/compile_budget.o"
clang "${flags[@]}" -c runtime/nvn/vulkan/shader_keys.c -o "$out/shader_keys.o"
clang "${flags[@]}" -c runtime/nvn/maxwell/translate.c -o "$out/translate.o"
clang "${flags[@]}" -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o "$out/digest.o"
for diagnostics in 0 1; do
    clang++ -std=c++17 "${flags[@]}" -DVK_SHADER_ARTIFACT_MAX_FILES=16 -DTOTK_DIAGNOSTICS=$diagnostics tools/test_shader_artifacts.cpp \
        runtime/nvn/vulkan/shader_artifact.cpp runtime/nvn/vulkan/shader_artifact_storage.cpp "$out/shaders.o" "$out/compile_budget.o" "$out/shader_keys.o" "$out/translate.o" "$out/digest.o" \
        -Wl,--gc-sections -lzstd -ldl -pthread -o "$out/test-$diagnostics"
    LARECOMP_NATIVE_LIB_DIR="$out" "$out/test-$diagnostics" "$out/unit-$diagnostics"
done
if [[ $sanitizer != thread ]]; then
    LARECOMP_NATIVE_LIB_DIR="$out" "${PYTHON:-python3}" tools/test_shader_artifact_runtime.py "$out/runtime"
    clang++ -std=c++17 -O1 -g -pthread tools/test_shader_artifact_storage.cpp runtime/nvn/vulkan/shader_artifact_storage.cpp \
        -ldl -Wl,--export-dynamic-symbol=pthread_create -o "$out/test-storage"
    "$out/test-storage" "$out/storage"
fi
