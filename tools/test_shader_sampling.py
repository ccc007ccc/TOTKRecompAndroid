#!/usr/bin/env python3
"""Validate new vertex sampling and retained gather correction through glslang and SPIR-V validation (dev)."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.work/totk-port/shader-sampling-tests'
out.mkdir(parents=True, exist_ok=True)
for name in ('vertex_snap', 'gather'):
    exe = out / name
    subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined',
                    str(root / f'tools/test_shader_{name}.c'), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(out)], check=True)
count = 0
for path in sorted(out.iterdir()):
    if path.suffix not in ('.vert', '.frag'): continue
    spv = path.with_suffix(path.suffix + '.spv')
    subprocess.run(['glslangValidator', '--target-env', 'vulkan1.3', '-V', str(path), '-o', str(spv)],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(spv)], check=True)
    count += 1
print(f'{count} sampling shaders validated; gather bias retained, vertex snap limited to 2D/array vertex sampling')
