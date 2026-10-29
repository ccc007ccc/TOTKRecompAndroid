#!/usr/bin/env python3
"""Validate interface derivation against real, locally generated canonical GLSL.

Uses existing audit_shader_variants --samples output. Does not prepare a game or
write cache/readiness files. Run in dev with PLUGIN SAMPLE_DIRECTORY OUTPUT.
"""
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import ctypes as c
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import time
from test_shader_interfaces import Compiler


def instructions(data):
    words = struct.unpack(f'<{len(data)//4}I', data)
    at = 5
    while at < len(words):
        count, opcode = words[at] >> 16, words[at] & 65535
        assert count and at + count <= len(words)
        yield opcode, words[at:at+count]
        at += count


def interface(data):
    types, decorations, variables = {}, {}, []
    for opcode, w in instructions(data):
        if opcode in [21, 22, 23, 32]:
            types[w[1]] = (opcode, *w[2:])
        if opcode == 71 and len(w) == 4:
            decorations.setdefault(w[1], {})[w[2]] = w[3]
        if opcode == 59:
            variables.append(w)
    result = []
    for w in variables:
        d = decorations.get(w[2], {})
        if 30 in d:
            typ = types[types[w[1]][2]]
            value = (types[typ[1]], typ[2]) if typ[0] == 23 else (typ, 1)
            result.append(('location', w[3], d[30], value))
        if 33 in d:
            result.append(('descriptor', w[3], d.get(34, 0), d[33]))
    return sorted(result)


def preserved(data):
    # Arithmetic, function calls and precision annotations keep their original
    # IDs and operands. Only interface pointer/load/store instructions may change.
    return Counter(w for op, w in instructions(data)
                   if op == 12 or op == 57 or 126 <= op <= 242 or (op == 71 and w[2] == 42))


def variant(source, stage, kinds):
    direction, prefix = ('in', 'a') if stage == 0 else ('out', 'c')
    pattern = rf'layout\(location = (\d+)\) {direction} vec4 {prefix}(\d+);'
    result = re.sub(pattern, lambda m: f'layout(location = {m[1]}) {direction} '
                    f'{["vec4", "ivec4", "uvec4"][kinds[int(m[1])]]} {prefix}{m[2]};', source)
    if stage == 1:
        result = re.sub(r'(  c(\d+)\[\d\] = )as_f\((r\d+)\);',
                        lambda m: f'{m[1]}{["as_f", "int", "uint"][kinds[int(m[2])]]}({m[3]});', result)
        if kinds[0]:
            result = re.sub(r'^layout\(std140, set = 1, binding = 19\) uniform Tint [^\n]*\n', '', result, flags=re.M)
            result = re.sub(r'^  if \(ltint\[0\]\.w > 0\.0f\) [^\n]*\n[^\n]*\n', '', result, flags=re.M)
            assert 'ltint' not in result
    return result


def main():
    plugin, directory, output = sys.argv[1:]
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    compiler = Compiler(plugin)
    sources = {}
    for path in sorted(Path(directory).glob('sample-*')):
        if path.suffix not in ['.vert', '.frag']:
            continue
        stage = 0 if path.suffix == '.vert' else 1
        source = path.read_text()
        direction, prefix = ('in', 'a') if stage == 0 else ('out', 'c')
        declarations = re.findall(rf'layout\(location = (\d+)\) {direction} ([iu]?vec4) {prefix}\d+;', source)
        if not declarations or any(typ != 'vec4' for _, typ in declarations):
            continue
        sources[(stage, source)] = (path.name, [int(loc) for loc, _ in declarations])
    assert sources
    started = time.monotonic()

    def check(item):
        (stage, source), (name, locations) = item
        api = (1 << 22) | (1 << 12)
        baseline = compiler.call(compiler.compile, source.encode(), stage, api, False)
        checked, derived_bytes, direct_bytes, derive_seconds, compile_seconds = 0, 0, 0, 0.0, 0.0
        patterns = set()
        for mode in range(3):
            kinds = [0] * 32
            for loc in locations:
                kinds[loc] = mode + 1 if mode < 2 else loc % 3
            patterns.add(tuple(kinds))
        for kinds in patterns:
            begin = time.monotonic()
            derived = compiler.call(compiler.derive, baseline, len(baseline)//4, stage, (c.c_uint8 * 32)(*kinds), api)
            derive_seconds += time.monotonic() - begin
            begin = time.monotonic()
            direct = compiler.call(compiler.compile, variant(source, stage, kinds).encode(), stage, api, False)
            compile_seconds += time.monotonic() - begin
            assert interface(derived) == interface(direct), name
            assert preserved(baseline) == preserved(derived), name
            key = hashlib.sha256(bytes(kinds)).hexdigest()[:12]
            (output / f'{name}-{key}.spv').write_bytes(derived)
            checked += 1; derived_bytes += len(derived); direct_bytes += len(direct)
        return dict(name=name, variants=checked, derived_bytes=derived_bytes, direct_bytes=direct_bytes,
                    derive_worker_seconds=derive_seconds, compile_worker_seconds=compile_seconds)

    with ThreadPoolExecutor(max_workers=4) as executor:
        rows = list(executor.map(check, sources.items()))
    report = dict(scope='sample_interface_validation_only', game_prepared=False,
                  canonical_sources=len(rows), seconds=time.monotonic()-started)
    for field in ['variants', 'derived_bytes', 'direct_bytes', 'derive_worker_seconds', 'compile_worker_seconds']:
        report[field] = sum(row[field] for row in rows)
    (output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    (output / 'cases.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
