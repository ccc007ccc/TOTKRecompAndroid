#!/usr/bin/env bash
# Run inside dev. Uses real mmap; Vulkan driver calls are simulated.
set -euo pipefail
cd "$(dirname "$0")/.."
SHARED_TEST_OUT=${1:-.work/totk-port/shared-memory-tests}
mkdir -p "$SHARED_TEST_OUT"
${PYTHON:-python3} - "$SHARED_TEST_OUT/physical_dispatch.inc" <<'PYCODE'
from pathlib import Path
import sys
text=Path('runtime/kernel.c').read_text()
a=text.index('typedef struct { uint64_t addr, size; uint32_t result; } PhysicalRelease;')
b=text.index('// ------------------------------------------------------------------ threads',a)
Path(sys.argv[1]).write_text('#ifdef __ANDROID__\n'+text[a:b])
PYCODE
${CC:-clang} -I"$SHARED_TEST_OUT" -D__ANDROID__ -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Iandroid/deps/out/include -Iandroid/deps/out/volk tools/test_shared_memory.c \
    -Wl,--gc-sections -pthread -o "$SHARED_TEST_OUT/test-shared-memory"
"$SHARED_TEST_OUT/test-shared-memory"
