#!/usr/bin/env bash
# Pure CPU lifetime/admission tests; all outputs are private, no phone actions.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-.work/shader-scheduling-tests}
mkdir -p "$out"
for sanitizer in address,undefined ${TOTK_SHADER_TSAN:+thread}; do
    flags=(-O1 -g -fsanitize="$sanitizer" -pthread -Iandroid/deps/out/include)
    clang "${flags[@]}" tools/test_compile_budget.c runtime/nvn/vulkan/compile_budget.c -o "$out/budget-$sanitizer"
    "$out/budget-$sanitizer"
    clang "${flags[@]}" tools/test_shader_module_cache.c runtime/nvn/vulkan/shader_module_cache.c runtime/nvn/vulkan/compile_budget.c -o "$out/modules-$sanitizer"
    "$out/modules-$sanitizer"
    for platform in host android; do
        extra=();if [[ $platform == android ]]; then extra=(-D__ANDROID__ -Iandroid/deps/out/volk); fi
        clang "${flags[@]}" "${extra[@]}" tools/test_shader_source_keys.c -o "$out/keys-$platform-$sanitizer"
        "$out/keys-$platform-$sanitizer"
    done
done
