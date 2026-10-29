#!/usr/bin/env bash
# Run inside dev; no phone or game files required.
set -euo pipefail
cd "$(dirname "$0")/.."
COPY_TEST_OUT=${1:-.work/totk-port/buffer-copy-tests}
mkdir -p "$COPY_TEST_OUT"
${CC:-clang} -D__ANDROID__ -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Iandroid/deps/out/include -Iandroid/deps/out/volk tools/test_buffer_copy.c \
    -Wl,--gc-sections -o "$COPY_TEST_OUT/test-buffer-copy"
"$COPY_TEST_OUT/test-buffer-copy" off
"$COPY_TEST_OUT/test-buffer-copy"
${PYTHON:-python3} tools/test_buffer_copy_commands.py "$COPY_TEST_OUT"
${PYTHON:-python3} tools/test_buffer_copy_uniforms.py "$COPY_TEST_OUT"
${PYTHON:-python3} tools/test_texture_notifications.py "$COPY_TEST_OUT"
