#!/bin/sh
# Build the desktop test plugin; Android uses the shaderprepare CMake target.
# Run in dev: sh tools/build_host_shader_compiler.sh OUTPUT_DIRECTORY
set -eu
cd "$(dirname "$0")/.."
SHADER_HOST_OUT=${1:?usage: tools/build_host_shader_compiler.sh OUTPUT_DIRECTORY}
mkdir -p "$SHADER_HOST_OUT"
"${CC:-clang}" -O2 -fPIC -c runtime/nvn/vulkan/shader_compiler.c \
    -o "$SHADER_HOST_OUT/shader_compiler.o"
"${CXX:-clang++}" -std=c++17 -O2 -shared -fPIC runtime/nvn/vulkan/shader_interface.cpp \
    "$SHADER_HOST_OUT/shader_compiler.o" -lshaderc_shared -lSPIRV-Tools-shared \
    -o "$SHADER_HOST_OUT/libshaderprepare.so"
