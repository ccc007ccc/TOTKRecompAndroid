#!/usr/bin/env bash
# Run inside dev; no phone or game data. Reuses the worker GPU fixture for combination coverage.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-.work/totk-port/descriptor-update-tests}
mkdir -p "$out"
"${PYTHON:-python3}" tools/test_descriptor_updates.py "$out/cpu"
"${PYTHON:-python3}" tools/gen_vk_command_worker.py --header android/deps/out/include/vulkan/vulkan_core.h --out "$out/generated"
glslc --target-env=vulkan1.1 tools/test_descriptor_updates.comp -o "$out/compute.spv"
spirv-val --target-env vulkan1.1 "$out/compute.spv"
${CC:-clang} -O2 -g -D__ANDROID__ -DTOTK_TEST_DESCRIPTOR_UPDATES=1 -Iandroid/deps/out/include -Iandroid/deps/out/volk \
    -I"$out/generated" tools/test_vk_command_worker_gpu.c runtime/nvn/vulkan/descriptor_update.c \
    runtime/nvn/vulkan/command_worker.c android/deps/out/volk/volk.c -pthread -ldl -o "$out/gpu"
"$out/gpu" "$out/compute.spv"
