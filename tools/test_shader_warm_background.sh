#!/usr/bin/env bash
# Run inside dev. Optional second argument also exercises cancellation against
# actual title/common requests, the production compiler and cache.
set -euo pipefail
cd "$(dirname "$0")/.."
WARM_JOB_OUT=${1:-.work/shader-background-tests}
mkdir -p "$WARM_JOB_OUT"
WARM_JOB_OUT=$(cd "$WARM_JOB_OUT" && pwd)
FLAGS=(-std=c++17 -O1 -g -fsanitize=address,undefined -pthread)
clang++ "${FLAGS[@]}" tools/test_shader_warm_job.cpp -o "$WARM_JOB_OUT/test-job"
"$WARM_JOB_OUT/test-job"
clang++ "${FLAGS[@]}" tools/test_shader_warm_job_api.cpp runtime/nvn/vulkan/shader_warm_job.cpp -o "$WARM_JOB_OUT/test-job-api"
"$WARM_JOB_OUT/test-job-api"
clang -O1 -g -fsanitize=address,undefined -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o "$WARM_JOB_OUT/digest.o"
clang++ "${FLAGS[@]}" tools/test_shader_warm_requests.cpp "$WARM_JOB_OUT/digest.o" -lzstd -o "$WARM_JOB_OUT/test-requests"
"$WARM_JOB_OUT/test-requests" "$WARM_JOB_OUT/requests"
clang++ "${FLAGS[@]}" -Iandroid/deps/out/include tools/test_shader_warm_observed.cpp runtime/nvn/vulkan/shader_warm_observed.cpp \
    "$WARM_JOB_OUT/digest.o" -lzstd -o "$WARM_JOB_OUT/test-observed"
"$WARM_JOB_OUT/test-observed" "$WARM_JOB_OUT/observed"
clang -O1 -g -fsanitize=address,undefined -c runtime/nvn/vulkan/compile_budget.c -o "$WARM_JOB_OUT/compile_budget.o"
clang++ "${FLAGS[@]}" tools/test_shader_prepare_queue.cpp "$WARM_JOB_OUT/compile_budget.o" -o "$WARM_JOB_OUT/test-queue"
"$WARM_JOB_OUT/test-queue"
if [[ $# -ge 2 ]]; then
    bash tools/test_shader_warm_prepare.sh "$WARM_JOB_OUT/integration" "$2" --cancel-background
fi
