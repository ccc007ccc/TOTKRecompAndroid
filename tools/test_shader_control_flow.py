#!/usr/bin/env python3
"""Run in dev; all generated files stay under the selected project directory."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=root / '.work/totk-port/scratch/shader-control-flow')
    parser.add_argument('--gpu', action='store_true', help='Also execute divergent branch fixtures on Vulkan with required validation')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cc', '-O2', str(root / 'tools/test_shader_control_flow.c'), '-lm', '-o', str(out / 'generate')], check=True)
    subprocess.run([str(out / 'generate'), str(out)], check=True)
    subprocess.run(['c++', '-O2', str(out / 'flow.cpp'), '-o', str(out / 'execute')], check=True)
    subprocess.run([str(out / 'execute')], check=True)

    def validate(path):
        spv = path.with_suffix(path.suffix + '.spv')
        subprocess.run(['glslc', '--target-env=vulkan1.3', '-O0', str(path), '-o', str(spv)], check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(spv)], check=True)

    paths = sorted(p for p in out.glob('flow-*') if p.suffix in ('.vert', '.frag', '.comp'))
    with ThreadPoolExecutor(max_workers=4) as workers:
        list(workers.map(validate, paths))
    print(f'{len(paths)} GLSL/SPIR-V control-flow shaders validated')
    if args.gpu:
        for fixture in range(4, 12):
            for stacks in range(2):
                for structured in range(2):
                    for explicit_next in range(2):
                        name=f'flow-{fixture}-{stacks}-{structured}-{explicit_next}.comp'
                        source=(out/name).read_text()
                        source=source.replace('layout(std140, set = 0, binding = 0) uniform CB0 { uvec4 cb0[16]; };',
                            'layout(std430, set=0, binding=0) readonly buffer Input { uint input_values[]; };\n'
                            'layout(std430, set=0, binding=1) writeonly buffer Output { uint output_values[]; };')
                        source=source.replace('layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;',
                                              'layout(local_size_x=64) in;')
                        source=source.replace('cb0[0][0]', 'input_values[gl_GlobalInvocationID.x]')
                        end=source.rfind('}')
                        source=source[:end]+'output_values[gl_GlobalInvocationID.x] = r1;\n'+source[end:]
                        path=out/('gpu-'+name);path.write_text(source);validate(path)
        executable=out/'execute-gpu'
        subprocess.run(['cc','-std=c11','-O2',str(root/'tools/test_vk_control_flow.c'),'-lvulkan','-o',str(executable)],check=True)
        subprocess.run([str(executable),str(out)],check=True)


if __name__ == '__main__':
    main()
