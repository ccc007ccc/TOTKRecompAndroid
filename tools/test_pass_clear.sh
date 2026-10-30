#!/usr/bin/env bash
# Run inside dev. Optional ROMFS is read only; this test never uses a phone.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-.work/totk-port/pass-clear-tests}
mkdir -p "$out"
"${PYTHON:-python3}" tools/test_pass_clear.py "$out/cpu"
${CC:-clang} -O1 -g -fsanitize=address,undefined tools/test_shader_depth_inventory.c \
    runtime/nvn/maxwell/translate.c -o "$out/depth-inventory-test"
"$out/depth-inventory-test"
"${PYTHON:-python3}" tools/gen_vk_command_worker.py --header android/deps/out/include/vulkan/vulkan_core.h --out "$out/generated"
${CC:-clang} -O2 -g -D__ANDROID__ -DTOTK_VK_COMMAND_WORKER=1 -DTOTK_DIAGNOSTICS=1 \
    -Iandroid/deps/out/include -Iandroid/deps/out/volk -I"$out/generated" \
    tools/test_vk_pass_clear.c runtime/nvn/vulkan/command_worker.c android/deps/out/volk/volk.c \
    -pthread -ldl -o "$out/gpu"
"$out/gpu"
if [[ $# -ge 2 ]]; then
    ${CC:-clang} -O2 -c runtime/nvn/maxwell/translate.c -o "$out/translate.o"
    ${CC:-clang} -O2 -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o "$out/digest.o"
    ${CXX:-clang++} -std=c++17 -O2 -Iandroid/deps/out/include -Ithird_party/d2r-digest/include \
        tools/audit_shader_depth.cpp "$out/translate.o" "$out/digest.o" -lzstd -o "$out/depth-inventory"
    "$out/depth-inventory" "$2"
fi
