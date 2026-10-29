#!/usr/bin/env bash
# Run inside dev. No phone or game data needed.
set -euo pipefail
cd "$(dirname "$0")/.."
TEXTURE_TEST_OUT=${1:-.work/totk-port/texture-registration-tests}
mkdir -p "$TEXTURE_TEST_OUT"
${CC:-clang} -D__ANDROID__ -D_GNU_SOURCE -O1 -g -Wno-format -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    tools/test_texture_registration.c -Wl,--gc-sections -pthread -o "$TEXTURE_TEST_OUT/test-texture-registration"
timeout 10 "$TEXTURE_TEST_OUT/test-texture-registration"
timeout 10 "$TEXTURE_TEST_OUT/test-texture-registration" off
timeout 10 "$TEXTURE_TEST_OUT/test-texture-registration" queued
timeout 10 "$TEXTURE_TEST_OUT/test-texture-registration" queued-default
${CC:-clang} -D__ANDROID__ -D_GNU_SOURCE -O1 -g -fsanitize=address,undefined \
    -Iandroid/deps/out/include -Iandroid/deps/out/volk tools/test_async_memory.c -pthread \
    -o "$TEXTURE_TEST_OUT/test-async-memory"
timeout 10 "$TEXTURE_TEST_OUT/test-async-memory"
${PYTHON:-python3} tools/test_deferred_resource_defaults.py "$TEXTURE_TEST_OUT"
