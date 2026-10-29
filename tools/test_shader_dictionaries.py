#!/usr/bin/env python3
"""Compare production queue/cache outputs with independent compilation (run in dev).

Usage: test_shader_dictionaries.py WORKER_EXECUTABLE OUTPUT [REAL_SAMPLE_DIRECTORY]
LARECOMP_NATIVE_LIB_DIR must contain the current libshaderprepare.so.
"""
import itertools
import json
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import sys
from audit_shader_interfaces import interface, preserved
from test_shader_interfaces import normalized, source


def compare(worker, samples, output, *, exact, optimized):
    env = os.environ.copy()
    if optimized:
        env['LARECOMP_VK_OPT'] = '1'
    else:
        env.pop('LARECOMP_VK_OPT', None)
    shared, direct = output / 'shared', output / 'direct'
    for directory, flags in [(shared, ['--families']), (direct, [])]:
        subprocess.run([str(worker), str(samples), str(directory), *flags], env=env, check=True)
    rows = (samples / 'families.txt').read_text().splitlines()
    def check(row):
        base, name, _ = row.split()
        a, b = [directory / (name + '.spv') for directory in [shared, direct]]
        for path in [a, b]:
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(path)], check=True)
        if exact:
            assert normalized(a) == normalized(b), name
        else:
            assert interface(a.read_bytes()) == interface(b.read_bytes()), name
            assert preserved(a.read_bytes()) == preserved((direct / (base + '.spv')).read_bytes()), name
    checked = 0
    with ThreadPoolExecutor(max_workers=4) as validators:
        for checked, _ in enumerate(validators.map(check, rows), 1):
            if checked % 64 == 0 or checked == len(rows):
                print(f'Validated and compared {checked}/{len(rows)} cached modules', flush=True)
    report = dict(scope='queue_and_cache_samples_only', game_prepared=False, checked=checked,
                  optimized=optimized, shared=json.loads((shared / 'report.json').read_text()),
                  direct=json.loads((direct / 'report.json').read_text()))
    (output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


def main():
    worker, output = [Path(arg).resolve() for arg in sys.argv[1:3]]
    samples = output / 'synthetic-samples'; samples.mkdir(parents=True, exist_ok=True)
    rows = []
    for stage in [0, 1]:
        ext = 'vert' if stage == 0 else 'frag'
        for kinds in itertools.product(range(3), repeat=2):
            name = f'{stage}-{kinds[0]}{kinds[1]}.{ext}'
            (samples / name).write_text(source(stage, kinds))
            rows.append(f'{stage}-00.{ext} {name} {kinds[0]}{kinds[1]}' + '0' * 30)
    (samples / 'families.txt').write_text('\n'.join(rows) + '\n')
    for optimized in [False, True]:
        compare(worker, samples, output / f'synthetic-{int(optimized)}', exact=True, optimized=optimized)
    if len(sys.argv) == 4:
        compare(worker, Path(sys.argv[3]).resolve(), output / 'real', exact=False, optimized=False)


if __name__ == '__main__':
    main()
