#!/usr/bin/env bash
# Run inside dev: bash tools/test_pipeline_cache.sh [OUTPUT_DIRECTORY]
set -euo pipefail
cd "$(dirname "$0")/.."
PIPELINE_TEST_OUT=${1:-.work/totk-port/pipeline-cache-tests}
mkdir -p "$PIPELINE_TEST_OUT"
PIPELINE_TEST_OUT=$(cd "$PIPELINE_TEST_OUT" && pwd)
${CC:-clang} -O1 -g -fsanitize=address,undefined -Iandroid/deps/out/include tools/test_pipeline_cache.c \
    -pthread -o "$PIPELINE_TEST_OUT/test-pipeline-cache"
"$PIPELINE_TEST_OUT/test-pipeline-cache" "$PIPELINE_TEST_OUT"
