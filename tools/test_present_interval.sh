#!/usr/bin/env bash
# Run inside dev. CPU only; no game files or phone.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-.work/totk-port/present-interval-tests}
mkdir -p "$out"
for diagnostics in 1 0; do
    ${CC:-clang} -D__ANDROID__ -D_GNU_SOURCE -DTOTK_DIAGNOSTICS=$diagnostics -O1 -g -Wno-format \
        -fsanitize=address,undefined -ffunction-sections -fdata-sections tools/test_present_interval.c \
        -Wl,--gc-sections -pthread -lm -o "$out/check-$diagnostics"
    "$out/check-$diagnostics"
    "$out/check-$diagnostics" off
    "$out/check-$diagnostics" force
done
