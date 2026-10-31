#!/usr/bin/env bash
# Run in dev: real guest-core queues, yield policy, wait attribution and reset.
set -euo pipefail
cd "$(dirname "$0")/.."
KERNEL_TEST_OUT=${1:-.work/totk-port/kernel-scheduling-tests}
mkdir -p "$KERNEL_TEST_OUT"
CC=${CC:-clang}
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
unset LARECOMP_NO_CORE0 LARECOMP_C0_GUEST_YIELD LARECOMP_C0_PRIORITY LARECOMP_LOG_KERNEL_WAITS
flags=(-O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections)
for diagnostics in 0 1; do
    binary="$KERNEL_TEST_OUT/affinity-$diagnostics"
    "$CC" "${flags[@]}" -D__ANDROID__ -DTOTK_DIAGNOSTICS="$diagnostics" \
        tools/test_kernel_affinity.c -Wl,--gc-sections -pthread -o "$binary"
    "$binary"
    for mode in --default --disabled --no-core0 --other-game; do "$binary" "$mode"; done
    "$binary" --trace-hold
    priority_binary="$KERNEL_TEST_OUT/priority-$diagnostics"
    "$CC" "${flags[@]}" -D__ANDROID__ -DTOTK_DIAGNOSTICS="$diagnostics" \
        tools/test_kernel_priority.c -Wl,--gc-sections -pthread -o "$priority_binary"
    for mode in default on off other-game; do "$priority_binary" "$mode"; done
    mutex_binary="$KERNEL_TEST_OUT/priority-mutex-$diagnostics"
    "$CC" "${flags[@]}" -D__ANDROID__ -DTOTK_DIAGNOSTICS="$diagnostics" \
        tools/test_kernel_priority_mutex.c -Wl,--gc-sections,--wrap=realloc -pthread -o "$mutex_binary"
    for mode in default on off other-game; do "$mutex_binary" "$mode"; done
done
"$CC" "${flags[@]}" -DTOTK_DIAGNOSTICS=1 tools/test_kernel_wait_probe.c \
    -Wl,--gc-sections -pthread -o "$KERNEL_TEST_OUT/wait-probe"
"$KERNEL_TEST_OUT/wait-probe"
"$KERNEL_TEST_OUT/wait-probe" --no-long
