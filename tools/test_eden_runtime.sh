#!/usr/bin/env bash
# Run in dev after sourcing android/dev_env.sh. No phone or game files needed.
set -euo pipefail
cd "$(dirname "$0")/.."
EDEN_TEST_OUT=${1:-.work/totk-port/eden-runtime-tests}
mkdir -p "$EDEN_TEST_OUT"
EDEN_TEST_OUT=$(cd "$EDEN_TEST_OUT" && pwd)
EDEN_PYTHON=${PYTHON:-python3}
EDEN_CC=${CC:-clang}
: "${ANDROID_NDK:?source android/dev_env.sh first}"
"$EDEN_PYTHON" tools/test_gather_policy.py "$EDEN_TEST_OUT/gather-policy"
"$EDEN_PYTHON" tools/test_shader_sampling.py
"$EDEN_PYTHON" tools/test_android_upload_caches.py "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
"$EDEN_PYTHON" tools/test_descriptor_plan.py "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
"$EDEN_CC" -O1 -g -D__ANDROID__ -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Iandroid/deps/out/include -Iandroid/deps/out/volk tools/test_async_memory.c \
    -Wl,--gc-sections -pthread -o "$EDEN_TEST_OUT/test-async-memory"
"$EDEN_TEST_OUT/test-async-memory"
bash tools/test_pipeline_cache.sh "$EDEN_TEST_OUT/pipeline"
bash tools/test_shader_runtime.sh "$EDEN_TEST_OUT/runtime"
