#!/usr/bin/env python3
"""Convert the pinned upstream list to portable shader requests (no game data).

R/B/T retain the raw vertex/fragment pairing and Vulkan-relevant numeric types.
Metal border/tile pipeline patches are not Vulkan shader variants: our renderer
uses its own sampler/render-pass implementations. Original rows remain in the
vendored list. This manifest prepares modules, not driver pipelines.
"""
from collections import Counter
import json
from pathlib import Path
import struct
import sys

from import_shader_warm_list import verified_list, warm_shaders

MAGIC = b'TKSWARM1'
RECORD = struct.Struct('<BBH32s32s32B8B8B')
PAIR, INFER_STAGE = 6, 255
TITLE = 1
# runtime/nvn/vertex_format.h: NVN vertex scalar types (not MTLPixelFormat).
NVN_UINT = {3, 8, 11, 15, 20, 23, 27, 32, 35, 39, 44, 47, 62}
NVN_SINT = {4, 9, 12, 16, 21, 24, 28, 33, 36, 40, 45, 48}
# Metal's public MTLPixelFormat values; nvn_mtl.m pfmt_class uses these cases.
MTL_UINT = {13, 23, 33, 53, 63, 73, 103, 113, 123, 91}
MTL_SINT = {14, 24, 34, 54, 64, 74, 104, 114, 124}
TYPE = {'float': 0, 'int': 1, 'sint': 1, 'uint': 2}


def vertex_kind(packed):
    fmt, stream = packed & 255, packed >> 27
    if not fmt or stream >= 16:
        return 0
    return 2 if fmt in NVN_UINT else 1 if fmt in NVN_SINT else 0


def render_types(key):
    data = bytes.fromhex(key)
    if len(data) != 544:
        raise ValueError('unsupported upstream render pipeline ABI')
    attr = [vertex_kind(value) for value in struct.unpack_from('<16I', data, 336)] + [0] * 16
    targets = struct.unpack_from('<8I', data, 216)
    output, assigned = [0] * 8, {}
    for i, fmt in enumerate(targets):
        if not fmt:
            continue
        location = data[528 + i] if data[162] else i
        if location == 255:
            continue
        if location >= 8:
            raise ValueError('upstream fragment output location outside Vulkan limits')
        kind = 2 if fmt in MTL_UINT else 1 if fmt in MTL_SINT else 0
        if location in assigned and assigned[location] != kind:
            raise ValueError('one upstream fragment output has conflicting numeric types')
        assigned[location] = output[location] = kind
    return attr, output


def pack(kind, key, paired='', attr=None, output=None, images=None):
    return RECORD.pack(kind, 0, 0, bytes.fromhex(key), bytes.fromhex(paired) if paired else bytes(32),
                       *(attr or [0] * 32), *(output or [0] * 8), *(images or [0] * 8))


def build(shared):
    requests, deferred = {}, Counter()
    def add(record, flags='m'):
        # The title subset is promoted even if its duplicate was first recorded
        # in gameplay. Dict insertion order otherwise follows upstream warm order.
        requests[record] = requests.get(record, 0) | (TITLE if 't' in flags else 0)
    for row, flags in shared.warm.items():
        needed = warm_shaders(row)
        if needed is None:
            deferred['unknown_pipeline_abi_or_custom_shader'] += 1
            continue
        if row[0] in ('R', 'B', 'T'):
            if [stage for _, stage in needed] != [0, 1]:
                deferred['unsupported_graphics_stage'] += 1
                continue
            attr, output = render_types(row[1])
            add(pack(PAIR, needed[0][0], needed[1][0], attr, output), flags)
        else:
            key, stage = needed[0]
            add(pack(stage, key), flags)
    for key, deps_json in sorted(shared.variants):
        deps = json.loads(deps_json)
        if deps.get('img'):
            # The pinned list has no image dependencies. Do not silently invent
            # a type mapping if a later upstream revision introduces them.
            deferred['image_dependency_requires_adapter'] += 1
            continue
        attr, output = [0] * 32, [0] * 8
        for group, kinds in (('attr', attr), ('fout', output)):
            for slot, name in deps.get(group, {}).items():
                location = int(slot)
                if not 0 <= location < len(kinds) or name not in TYPE:
                    raise ValueError('unsupported upstream interface dependency')
                kinds[location] = TYPE[name]
        add(pack(INFER_STAGE, key, attr=attr, output=output))
    ordered = sorted(requests.items(), key=lambda item: not (item[1] & TITLE))
    blob = bytearray(MAGIC + struct.pack('<II', RECORD.size, len(ordered)))
    for record, flags in ordered:
        blob += record[:1] + bytes([flags]) + record[2:]
    return bytes(blob), dict(requests=len(ordered), title_requests=sum(bool(flags & TITLE) for _, flags in ordered),
                             record_bytes=RECORD.size, bytes=len(blob), deferred=dict(deferred),
                             driver_pipelines=False)


def main(argv):
    if len(argv) != 1:
        raise SystemExit('usage: build_shader_warm_manifest.py OUTPUT.bin')
    shared, source = verified_list()
    blob, summary = build(shared)
    path = Path(argv[0]); path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    summary['upstream_commit'] = source['commit']
    path.with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main(sys.argv[1:])
