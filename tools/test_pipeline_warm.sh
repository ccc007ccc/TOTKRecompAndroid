#!/usr/bin/env bash
# Run in dev. This tests file/thread behavior; GPU coverage is a separate check.
set -euo pipefail
cd "$(dirname "$0")/.."
PIPE_WARM_TEST=${1:-.work/pipeline-warm-tests}
mkdir -p "$PIPE_WARM_TEST"
PIPE_WARM_TEST=$(cd "$PIPE_WARM_TEST" && pwd)
clang -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Iandroid/deps/out/include -Ithird_party/d2r-digest/include \
    tools/test_pipeline_warm.c runtime/nvn/vulkan/pipeline_state.c runtime/nvn/vulkan/compile_budget.c runtime/nvn/vulkan/pipeline_seed.c third_party/d2r-digest/digest.c -Wl,--gc-sections -pthread -lvulkan -o "$PIPE_WARM_TEST/test-pipeline-warm"
"$PIPE_WARM_TEST/test-pipeline-warm" "$PIPE_WARM_TEST"
python tools/test_shader_jobs.py "$PIPE_WARM_TEST/jobs"
