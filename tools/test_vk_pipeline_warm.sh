#!/usr/bin/env bash
# Run in dev: [OUTPUT] [--android]. Android mode builds standalone phone tests.
set -euo pipefail
cd "$(dirname "$0")/.."
PIPE_GPU_OUT=${1:-.work/pipeline-warm-gpu}
mkdir -p "$PIPE_GPU_OUT"
PIPE_GPU_OUT=$(cd "$PIPE_GPU_OUT" && pwd)
cat > "$PIPE_GPU_OUT/test.vert" <<'GLSL'
#version 450
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}
GLSL
cat > "$PIPE_GPU_OUT/test.frag" <<'GLSL'
#version 450
layout(set=1,binding=0) uniform C {vec4 bias;};
layout(location=0) out vec4 color;
void main(){color=vec4(gl_FragCoord.xy+bias.xy,bias.zw);}
GLSL
glslc --target-env=vulkan1.1 "$PIPE_GPU_OUT/test.vert" -o "$PIPE_GPU_OUT/test.vert.spv"
glslc --target-env=vulkan1.1 "$PIPE_GPU_OUT/test.frag" -o "$PIPE_GPU_OUT/test.frag.spv"
cat > "$PIPE_GPU_OUT/test.comp" <<'GLSL'
#version 450
layout(local_size_x_id=0,local_size_y_id=1,local_size_z_id=2) in;
layout(std430,set=0,binding=0) buffer R {uint value[];};
void main(){uvec3 p=gl_LocalInvocationID;uint i=gl_LocalInvocationIndex;value[i]=31u+p.x+10u*p.y+100u*p.z;}
GLSL
glslc --target-env=vulkan1.1 "$PIPE_GPU_OUT/test.comp" -o "$PIPE_GPU_OUT/test.comp.spv"
PIPE_GPU_CC=clang
FLAGS=(-O2 -g -ffunction-sections -fdata-sections -Iandroid/deps/out/include -Ithird_party/d2r-digest/include)
SOURCES=(tools/test_vk_pipeline_warm.c runtime/nvn/vulkan/pipeline_state.c runtime/nvn/vulkan/compile_budget.c runtime/nvn/vulkan/pipeline_warm.c runtime/nvn/vulkan/pipeline_seed.c third_party/d2r-digest/digest.c runtime/nvn/vulkan/rpcompat.c)
LIBS=(-Wl,--gc-sections -lvulkan -pthread)
if [[ ${2:-} == --android ]]; then
    source android/dev_env.sh
    PIPE_GPU_CC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    FLAGS+=(-Iandroid/deps/out/volk -Wno-nullability-completeness)
    SOURCES+=(android/deps/out/volk/volk.c)
    LIBS=(-Wl,--gc-sections -ldl -llog -pthread)
fi
"$PIPE_GPU_CC" "${FLAGS[@]}" "${SOURCES[@]}" "${LIBS[@]}" -o "$PIPE_GPU_OUT/test-gpu"
if [[ ${2:-} != --android ]]; then
    unset LARECOMP_SHADER_WARM LARECOMP_PIPELINE_WARM LARECOMP_NO_DYN_UBO
    "$PIPE_GPU_OUT/test-gpu" --record "$PIPE_GPU_OUT" "$PIPE_GPU_OUT/test.vert.spv" "$PIPE_GPU_OUT/test.frag.spv" "$PIPE_GPU_OUT/test.comp.spv"
    "$PIPE_GPU_OUT/test-gpu" --render "$PIPE_GPU_OUT" "$PIPE_GPU_OUT/test.vert.spv" "$PIPE_GPU_OUT/test.frag.spv" "$PIPE_GPU_OUT/test.comp.spv"
    for stage in vert frag comp; do spirv-val --target-env vulkan1.1 "$PIPE_GPU_OUT/test.$stage.spv"; done
    "$PIPE_GPU_CC" "${FLAGS[@]}" -Dvk_shader_cache_namespace=fixture_cache_namespace -Dvk_shader_cache_load_key=fixture_disk_load \
        -c runtime/nvn/vulkan/shaders.c -o "$PIPE_GPU_OUT/shaders.o"
    "$PIPE_GPU_CC" "${FLAGS[@]}" -DTOTK_TEST_MODULE_REUSE=1 "${SOURCES[@]}" runtime/nvn/vulkan/shader_module_cache.c "$PIPE_GPU_OUT/shaders.o" \
        "${LIBS[@]}" -lzstd -ldl -o "$PIPE_GPU_OUT/test-reuse"
    "$PIPE_GPU_OUT/test-reuse" --render-seed "$PIPE_GPU_OUT" "$PIPE_GPU_OUT/test.vert.spv" "$PIPE_GPU_OUT/test.frag.spv" "$PIPE_GPU_OUT/test.comp.spv"
fi
