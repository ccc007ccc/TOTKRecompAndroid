#!/usr/bin/env bash
# Run inside dev. Optional second argument: a locally owned RomFS image.
set -euo pipefail
cd "$(dirname "$0")/.."
WARM_LIST_OUT=${1:-.work/shader-warm-list-tests}
mkdir -p "$WARM_LIST_OUT"
WARM_LIST_OUT=$(cd "$WARM_LIST_OUT" && pwd)
clang -O2 -fPIC -shared -Ithird_party/d2r-digest/include third_party/d2r-digest/digest.c -o "$WARM_LIST_OUT/libdigest.so"
export TOTK_TEST_DIGEST_LIB="$WARM_LIST_OUT/libdigest.so"
python tools/test_shader_warm_list.py
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined tools/test_shader_archive.cpp -lzstd -o "$WARM_LIST_OUT/test-archive"
"$WARM_LIST_OUT/test-archive" ${2:+"$2"}
