#!/usr/bin/env bash
# Run inside dev. CPU tests use driver doubles; GPU test requires Vulkan 1.3 + validation.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-.work/totk-port/vk-command-worker-tests}
mkdir -p "$out"
python=${PYTHON:-python3}
cc=${CC:-clang}
"$python" tools/test_vk_command_worker_generator.py
"$python" tools/gen_vk_command_worker.py --header android/deps/out/include/vulkan/vulkan_core.h --out "$out/generated"
includes=(-Iandroid/deps/out/include -Iandroid/deps/out/volk -I"$out/generated")
sources=(runtime/nvn/vulkan/command_worker.c android/deps/out/volk/volk.c)
wraps=(-Wl,--wrap=malloc,--wrap=free,--wrap=pthread_create,--wrap=pthread_cond_wait)
"$cc" -O1 -g -fsanitize=address,undefined "${includes[@]}" tools/test_vk_command_worker.c "${sources[@]}" \
    "${wraps[@]}" -pthread -ldl -o "$out/cpu"
"$out/cpu"
"$cc" -O1 -g -D__ANDROID__ -DTOTK_VK_COMMAND_WORKER=1 -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections "${includes[@]}" tools/test_async_memory.c "${sources[@]}" \
    -Wl,--gc-sections,--wrap=pthread_cond_wait -pthread -ldl -o "$out/async"
"$out/async"
glslc --target-env=vulkan1.1 tools/test_vk_command_worker.comp -o "$out/compute.spv"
spirv-val --target-env vulkan1.1 "$out/compute.spv"
"$cc" -O2 -g "${includes[@]}" tools/test_vk_command_worker_gpu.c "${sources[@]}" \
    -pthread -ldl -o "$out/gpu"
"$out/gpu" "$out/compute.spv"
# TSAN is an explicit extra pass; its availability/address-space requirements vary
# by host. A requested pass fails normally on either a race or an unsupported host.
if [[ ${TOTK_WORKER_TSAN:-0} == 1 ]]; then
    "$cc" -O1 -g -fsanitize=thread "${includes[@]}" tools/test_vk_command_worker.c "${sources[@]}" \
        "${wraps[@]}" -pthread -ldl -o "$out/cpu-tsan"
    "$out/cpu-tsan"
    "$cc" -O1 -g -D__ANDROID__ -DTOTK_VK_COMMAND_WORKER=1 -fsanitize=thread \
        -ffunction-sections -fdata-sections "${includes[@]}" tools/test_async_memory.c "${sources[@]}" \
        -Wl,--gc-sections,--wrap=pthread_cond_wait -pthread -ldl -o "$out/async-tsan"
    "$out/async-tsan"
fi
