#!/usr/bin/env bash
# Cross-build inside dev. Run the result on the phone with LD_LIBRARY_PATH=lib
# and LARECOMP_NATIVE_LIB_DIR pointing to the matching compiler plugin folder.
set -euo pipefail
cd "$(dirname "$0")/.."
source android/dev_env.sh
PHONE_WARM_OUT=${1:-.work/shader-phone-warm}
mkdir -p "$PHONE_WARM_OUT"
PHONE_WARM_OUT=$(cd "$PHONE_WARM_OUT" && pwd)
PHONE_WARM_CC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
PHONE_WARM_CXX="${PHONE_WARM_CC}++"
FLAGS=(-O2 -ffunction-sections -fdata-sections -Wno-nullability-completeness -Iandroid/deps/out/include -Ithird_party/d2r-digest/include -Iandroid/deps/out/volk)
cmake -DSHADER_CACHE_ROOT="$PWD" -DSHADER_CACHE_HEADER="$PHONE_WARM_OUT/shader_cache_contract.h" \
    -P tools/generate_shader_cache_contract.cmake
"$PHONE_WARM_CC" "${FLAGS[@]}" -DHAVE_SHADER_CACHE_CONTRACT=1 -I"$PHONE_WARM_OUT" \
    -c runtime/nvn/vulkan/shaders.c -o "$PHONE_WARM_OUT/shaders.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -c runtime/nvn/vulkan/compile_budget.c -o "$PHONE_WARM_OUT/compile_budget.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -c runtime/nvn/vulkan/shader_keys.c -o "$PHONE_WARM_OUT/shader_keys.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -c runtime/nvn/vulkan/pipeline_seed.c -o "$PHONE_WARM_OUT/pipeline_seed.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -c runtime/nvn/vulkan/pipeline_state.c -o "$PHONE_WARM_OUT/pipeline_state.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -c runtime/nvn/vulkan/pipeline_warm.c -o "$PHONE_WARM_OUT/pipeline_warm.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -c runtime/nvn/maxwell/translate.c -o "$PHONE_WARM_OUT/translate.o"
"$PHONE_WARM_CC" "${FLAGS[@]}" -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o "$PHONE_WARM_OUT/digest.o"
"$PHONE_WARM_CXX" -std=c++17 "${FLAGS[@]}" tools/test_shader_warm_prepare.cpp runtime/nvn/vulkan/shader_warm.cpp runtime/nvn/vulkan/shader_artifact.cpp runtime/nvn/vulkan/shader_artifact_storage.cpp \
    "$PHONE_WARM_OUT/shaders.o" "$PHONE_WARM_OUT/compile_budget.o" "$PHONE_WARM_OUT/shader_keys.o" "$PHONE_WARM_OUT/pipeline_seed.o" "$PHONE_WARM_OUT/pipeline_state.o" "$PHONE_WARM_OUT/pipeline_warm.o" "$PHONE_WARM_OUT/translate.o" "$PHONE_WARM_OUT/digest.o" \
    -Wl,--gc-sections -Landroid/deps/out/lib -lzstd -ldl -llog -o "$PHONE_WARM_OUT/test-warm"
"$PHONE_WARM_CXX" -std=c++17 "${FLAGS[@]}" tools/audit_shader_warm_plan.cpp \
    "$PHONE_WARM_OUT/shaders.o" "$PHONE_WARM_OUT/compile_budget.o" "$PHONE_WARM_OUT/shader_keys.o" "$PHONE_WARM_OUT/pipeline_seed.o" "$PHONE_WARM_OUT/pipeline_state.o" "$PHONE_WARM_OUT/pipeline_warm.o" "$PHONE_WARM_OUT/translate.o" "$PHONE_WARM_OUT/digest.o" \
    -Wl,--gc-sections -Landroid/deps/out/lib -lzstd -ldl -llog -o "$PHONE_WARM_OUT/audit-warm-cache"
"$PHONE_WARM_CC" "${FLAGS[@]}" -DHAVE_SHADER_CACHE_CONTRACT=1 -I"$PHONE_WARM_OUT" tools/audit_shader_cache.c \
    -Wl,--gc-sections -Landroid/deps/out/lib -lzstd -ldl -llog -o "$PHONE_WARM_OUT/audit-cache"
"$PHONE_WARM_CC" -O2 tools/test_shader_compiler_compat.c -ldl -o "$PHONE_WARM_OUT/test-compiler"
"$PYTHON" tools/build_shader_warm_manifest.py "$PHONE_WARM_OUT/manifest.bin"
"$PYTHON" - "$PHONE_WARM_OUT" <<'PY'
from pathlib import Path
import os,shutil,sys
root=Path(sys.argv[1]);(root/'lib').mkdir(exist_ok=True)
for name in ('libshaderc_shared.so','libzstd.so'):
    shutil.copyfile(Path('android/deps/out/lib')/name,root/'lib'/name)
# The application links libc++ statically; this standalone NDK test uses its
# shared runtime, from the same NDK rather than assuming it lives in the APK.
shutil.copyfile(Path(os.environ['ANDROID_NDK'])/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so',root/'lib/libc++_shared.so')
PY
# Build from the same current sources/dependency that generated the cache ID.
# An arbitrary old Debug APK can contain a different compiler implementation;
# copying its plugin would write that implementation's output under the new ID.
"$PHONE_WARM_CC" "${FLAGS[@]}" -fPIC -fvisibility=hidden -c runtime/nvn/vulkan/shader_compiler.c \
    -o "$PHONE_WARM_OUT/compiler.o"
"$PHONE_WARM_CXX" -std=c++17 "${FLAGS[@]}" -fPIC -fvisibility=hidden -shared \
    runtime/nvn/vulkan/shader_interface.cpp "$PHONE_WARM_OUT/compiler.o" \
    -Landroid/deps/out/lib -lshaderc_shared -Wl,-z,max-page-size=16384 \
    -o "$PHONE_WARM_OUT/lib/libshaderprepare.so"
