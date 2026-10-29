#!/bin/sh
# Run in dev. No game assets needed; all shader sources use the production emitter.
set -eu
cd "$(dirname "$0")/.."
out=${1:-build/test_vk_multisample}
mkdir -p "$out"
cc=${CC:-clang}
$cc -O1 -g -fsanitize=address,undefined tools/test_shader_multisample.c -lm -o "$out/generate"
"$out/generate" "$out"
cat > "$out/fill.vert" <<'GLSL'
#version 450
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}
GLSL
cat > "$out/fill.frag" <<'GLSL'
#version 450
layout(location=0) out vec4 color;
void main(){color=vec4(floor(gl_FragCoord.xy),gl_SampleID,10+gl_SampleID);}
GLSL
for source in "$out"/*.vert "$out"/*.frag "$out"/read-*.comp; do
    glslc --target-env=vulkan1.1 "$source" -o "$source.spv"
    spirv-val --target-env vulkan1.1 "$source.spv"
done
$cc -std=c11 -O2 -Wall -Wextra tools/test_vk_multisample.c -lvulkan -o "$out/test-gpu"
"$out/test-gpu" "$out/fill.vert.spv" "$out/fill.frag.spv" "$out/read-0.comp.spv" "$out/read-1.comp.spv"
