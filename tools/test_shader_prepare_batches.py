#!/usr/bin/env python3
"""Exercise a family larger than 2048 and independent compute image families.

Run in dev: test_shader_prepare_batches.py WORKER PLUGIN OUTPUT [SURFACE_SOURCES]
The optional surface directory comes from test_shader_images.cpp. No game data
is copied into the repository; without it only generated vertex sources are used.
"""
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
from test_shader_interfaces import Compiler, normalized


def vertex(kinds):
    types = ['vec4', 'ivec4', 'uvec4']
    declarations = '\n'.join(f'layout(location={i}) in {types[k]} a{i};' for i, k in enumerate(kinds))
    loads = [f'floatBitsToUint(a{i})' if k == 0 else f'uvec4(a{i})' for i, k in enumerate(kinds)]
    return f'''#version 450
{declarations}
layout(location=0) flat out uvec4 result;
void main(){{uvec4 r = {' ^ '.join(loads)};
result = r; gl_Position = vec4(uintBitsToFloat(r.x), 0, 0, 1);}}
'''


def main():
    worker, plugin, output = [Path(p).resolve() for p in sys.argv[1:4]]
    samples = output / 'sources'; samples.mkdir(parents=True, exist_ok=True)
    rows = []
    for index, kinds in enumerate(itertools.product(range(3), repeat=7)):
        name = f'vertex-{index:04}.vert'
        (samples / name).write_text(vertex(kinds))
        rows.append(f'vertex-0000.vert {name} ' + ''.join(map(str, kinds)) + '0' * 25)
    surfaces = []
    if len(sys.argv) == 5:
        for source in sorted(Path(sys.argv[4]).glob('*.comp')):
            (samples / source.name).write_bytes(source.read_bytes())
            surfaces.append(source.name)
            # Image types are independent bases, never interface derivations.
            rows.append(f'{source.name} {source.name} ' + '0' * 32)
    (samples / 'families.txt').write_text('\n'.join(rows) + '\n')
    env = os.environ.copy(); env.pop('LARECOMP_VK_OPT', None)
    env['LARECOMP_NATIVE_LIB_DIR'] = str(plugin.parent)
    subprocess.run([str(worker), str(samples), str(output / 'cache-test'), '--families'], env=env, check=True)
    report = json.loads((output / 'cache-test/report.json').read_text())
    assert report['sources'] == 2187 + len(surfaces)
    compiler = Compiler(plugin)
    # Compare both sides of the batch boundary with an independent compilation.
    for name in [f'vertex-{n:04}.vert' for n in [0, 1, 2047, 2048, 2186]] + surfaces:
        stage = 5 if name.endswith('.comp') else 0
        binary = compiler.call(compiler.compile, (samples / name).read_bytes(), stage, (1 << 22) | (3 << 12), False)
        direct = output / (name + '.direct.spv'); direct.write_bytes(binary)
        cached = output / 'cache-test' / (name + '.spv')
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(cached)], check=True)
        if stage == 5:
            assert binary == cached.read_bytes(), name
        else:
            assert normalized(direct) == normalized(cached), name
    report['boundary_comparisons'] = 5
    report['independent_compute_sources'] = len(surfaces)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: 2187-member vertex family across batches; {len(surfaces)} independent compute sources; production loads and direct comparisons')


if __name__ == '__main__':
    main()
