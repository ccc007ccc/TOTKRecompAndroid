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
subprocess.run([str(out / 'check'), 'marks'], check=True)
subprocess.run([str(out / 'check'), 'off'], check=True)

base = 0x7000000000
patches, pre = patches_for(BUILD_TOTK_1_4_3), prepatches_for(BUILD_TOTK_1_4_3)
assert len(patches) == 11 and len(pre) == 12
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
    print('23 patch sites and their neighboring guards match the private 1.4.3 image; changed inputs rejected')

# Compile the production lifter's actual fade replacements, including the original FCVTZS. Check saturation,
# non-finite inputs, paused/negative values, both destinations and the opt-out in fresh processes.
fade_sites = ((0xa7ac4, 8), (0x95dd24, 8), (0x6155e4, 10), (0x615660, 10))
image = bytearray(0x95dd28)
for a, rd in fade_sites:
    struct.pack_into('<I', image, a, 0x1e380000 | rd)
g = SimpleNamespace(base=base, nso=SimpleNamespace(image=image), lifter=lifter.Lifter(base, 'c'))
fixture = ('#define main timing_main\n#include "' + str(root / 'tools/test_totk_framepace.c') +
           '"\n#undef main\n#include "' + str(root / 'runtime/simd.h') + '"\n')
for i, (a, rd) in enumerate(fade_sites):
    fixture += f'static void fade_{i}(Cpu *c) ' + patches[base + a](g, base + a) + '\n'
fixture += '''
int main(int argc, char **argv) {
    bool off = argc > 1;
    if (off) setenv("LARECOMP_FRAMEPACE", "0", 1);
    void (*fns[])(Cpu *) = {fade_0, fade_1, fade_2, fade_3};
    const unsigned regs[] = {8, 8, 10, 10};
    const float values[] = {0, -0.0f, 0.5f, 0.999f, -0.5f, -1.9f, 2.1f, NAN, INFINITY, -INFINITY};
    const uint32_t normal[] = {0, 0, 0, 0, 0, UINT32_MAX, 2, 0, INT32_MAX, (uint32_t)INT32_MIN};
    for (unsigned f = 0; f < 4; ++f) for (unsigned v = 0; v < 10; ++v) {
        Cpu c = {0}, expected; c.nzcv = 0xa0000000; frame_set_s(&c, 0, values[v]);
        expected = c; expected.x[regs[f]] = !off && (v == 2 || v == 3) ? 1 : normal[v];
        fns[f](&c); assert(!memcmp(&c, &expected, sizeof c));
    }
    puts("Generated fades: positive step, pause, negatives, NaN/saturation, state preservation, opt-out passed");
}
'''
source = out / 'fade-generated.c'
source.write_text(fixture)
subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(source), '-pthread', '-lm', '-o', str(out / 'fade-check')], check=True)
subprocess.run([str(out / 'fade-check')], check=True)
subprocess.run([str(out / 'fade-check'), 'off'], check=True)

# Execute the real controller merge block emitted by Module: hook, MOV, interval store, division and FPS store.
# The RET terminates this small fixture after the production block, without executing unrelated frame-loop code.
controller_words = {0x2db238: 0x52800788, 0x2db23c: 0xb9402a8a, 0x2db240: 0xb9004295,
                    0x2db244: 0x1ad50d09, 0x2db248: 0x6b0a013f, 0x2db24c: 0xb9005289,
                    0x2db250: 0xd65f03c0, 0x2db280: 0xb9008a75, 0x2db2d0: 0x3902a115}
image = bytearray(0x2db2d4)
for a, word in controller_words.items():
    struct.pack_into('<I', image, a, word)
m = Module.__new__(Module)
m.base = m.lo = base; m.hi = base + len(image)
m.nso = SimpleNamespace(image=image); m.words = struct.unpack(f'<{len(image)//4}I', image)
m.lifter = lifter.Lifter(base, 'c'); m.cache = {}; m.kinds = bytearray(len(m.words)); m.stats = Counter()
m.patches = patches; m.pre_patches = pre; m.setjmps = set()
lifter.DYN_BASE = False
controller = '\n'.join(m.emit_function(base + 0x2db238, base + 0x2db254, {base + 0x2db238}, {}, {}))
assert controller.index('rt_totk_frame_hook(c, 6)') < controller.index('0x40')
source = out / 'controller-generated.c'
source.write_text('#define main timing_main\n#include "' + str(root / 'tools/test_totk_framepace.c') +
                  '"\n#undef main\n#define FN_ENTER(a) ((void)(a))\n' + controller + '''
int main(int argc, char **argv) {
    bool off = argc > 1;
    if (off) setenv("LARECOMP_FRAMEPACE", "0", 1);
    const int modes[] = {1, 3, 2, 4};
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned requested = 1; requested <= 4; ++requested) {
        g_settings.fps_mode = modes[mode];
        uint32_t object[32] = {0}; Cpu c = {0}; c.x[20] = (uintptr_t)object; c.x[21] = requested;
        f_70002db238(&c);
        unsigned interval = off ? requested : mode == 0 ? 2 : 1;
        assert(c.x[8] == 60 && c.x[21] == interval);
        assert(object[0x40/4] == interval && object[0x50/4] == 60/interval);
    }
    if (!off) {
        setenv("LARECOMP_TOTK_CUTSCENE_MARKS", "1", 1);
        uint32_t object[32] = {0}; Cpu c = {0}; c.x[20] = (uintptr_t)object; c.x[21] = 3;
        rt_totk_frame_hook(&c, 7); f_70002db238(&c);
        assert(object[0x40/4] == 2 && object[0x50/4] == 30);
    }
    puts("Generated controller: saved interval/FPS follow target and cutscene; opt-out retains game choice");
}
''')
subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(source), '-pthread', '-lm', '-o', str(out / 'controller-check')], check=True)
subprocess.run([str(out / 'controller-check')], check=True)
subprocess.run([str(out / 'controller-check'), 'off'], check=True)

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
