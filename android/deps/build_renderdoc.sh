#!/usr/bin/env bash
# Run inside distrobox dev. Build the optional Android capture layer.
set -euo pipefail
cd "$(dirname "$0")/../.."
source android/dev_env.sh
src="$TOTK_ROOT/.work/totk-port/renderdoc-v1.45"
build="$TOTK_ROOT/.work/totk-port/renderdoc-android-build"
if [[ ! -d "$src/.git" ]]; then
    git clone --depth 1 --branch v1.45 https://github.com/baldurk/renderdoc.git "$src"
fi
[[ $(git -C "$src" rev-parse HEAD) == 2fc0bc04cb95499635f63986a55bc6f67849dd9f ]]
patch="$TOTK_ROOT/android/deps/patches/renderdoc-memfd.patch"
if ! git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$src" apply --check "$patch"
    git -C "$src" apply "$patch"
fi
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_ANDROID=ON -DENABLE_QRENDERDOC=OFF -DENABLE_PYRENDERDOC=OFF \
    -DENABLE_RENDERDOCCMD=OFF -DENABLE_GL=OFF -DENABLE_VULKAN=ON \
    -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384
cmake --build "$build" --target renderdoc -j "${JOBS:-4}"
mkdir -p android/deps/out/lib
"$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded \
    "$build/lib/libVkLayer_GLES_RenderDoc.so" -o android/deps/out/lib/libVkLayer_GLES_RenderDoc.so
