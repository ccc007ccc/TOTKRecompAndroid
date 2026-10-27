#!/usr/bin/env python3
"""CPU timing regressions and generation guards; optionally validate --image <private main-image.bin>."""
import argparse
from collections import Counter
from pathlib import Path
import struct
import subprocess
from types import SimpleNamespace

import lifter
from gen import Module
from patches import BUILD_TOTK_1_4_3, patches_for, prepatches_for

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / '.work/totk-port/framepace-tests'
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(root / 'tools/test_totk_framepace.c'), '-pthread', '-lm', '-o', str(out / 'check')], check=True)
subprocess.run([str(out / 'check')], check=True)
subprocess.run([str(out / 'check'), 'off'], check=True)

base = 0x7000000000
patches, pre = patches_for(BUILD_TOTK_1_4_3), prepatches_for(BUILD_TOTK_1_4_3)
assert len(patches) == 7 and len(pre) == 7
assert not prepatches_for('unknown') and not patches_for('unknown')
assert not prepatches_for('de0b6ac4efc1dda7f80f6d7598ec7a7d')
if args.image:
    image = args.image.read_bytes()
    for mode in ('c', 'asm'):
        g = SimpleNamespace(base=base, nso=SimpleNamespace(image=image), lifter=lifter.Lifter(base, mode))
        for a, patch in {**patches, **pre}.items():
            patch(g, a)
        # All version guards must reject a different instruction stream.
        g.nso.image = bytes(len(image))
        for a, patch in {**patches, **pre}.items():
            try: patch(g, a)
            except AssertionError: pass
            else: raise AssertionError(f'unguarded site {a:x}')
    print('14 patch sites and their neighboring guards match the private 1.4.3 image; changed inputs rejected')

# Test the actual generator's BL path: a pre-hook must preserve the original call, callee reference and return address.
for a, word in ((0x700024944c, 0x94015e7d), (0x700028bc98, 0x9400546a)):
    image = bytearray(a - base + 8)
    struct.pack_into('<II', image, a - base, word, 0xd65f03c0)
    m = Module.__new__(Module)
    m.base = m.lo = base; m.hi = base + 0x600000
    m.nso = SimpleNamespace(image=image)
    m.words = struct.unpack(f'<{len(image)//4}I', image)
    m.lifter = lifter.Lifter(base, 'c'); m.cache = {}; m.kinds = bytearray(len(m.words)); m.stats = Counter()
    m.patches = patches; m.pre_patches = pre; m.setjmps = set()
    lifter.DYN_BASE = True
    body = '\n'.join(m.emit_function(a, a + 8, {a}, {}, {}))
    assert body.index('rt_totk_frame_hook(c, 4)') < body.index('f_70002a0e40(c)')
    assert f'c->x[30] = (GB + {hex(a + 4 - base)}ull);' in body
    assert 0x70002a0e40 in m._refs
print('pre-instruction hooks retain generated direct calls, LR, and callee references')
