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


if __name__ == '__main__':
    main()
