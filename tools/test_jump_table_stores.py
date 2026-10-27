#!/usr/bin/env python3
"""Execute synthetic local table branches with intervening FP loads/stores (dev, Clang)."""
from collections import Counter
from pathlib import Path
import struct
import subprocess
from types import SimpleNamespace

import lifter
from gen import Module

root = Path(__file__).resolve().parents[1]
out = root / '.work/totk-port/jump-table-store-tests'
out.mkdir(parents=True, exist_ok=True)
base = 0x7000000000
blocks = []
for i, middle in enumerate((0x2d1023e9, 0xbd0013e9, 0xbd4013e9)):
    # add x9,x10,x11; stp/str/ldr s9; br x9; ret; mov x0,#7; ret
    words = (0x8b0b0149, middle, 0xd61f0120, 0xd65f03c0, 0xd28000e0, 0xd65f03c0)
    entry = base + i * 0x100
    m = Module.__new__(Module)
    m.base = base; m.lo = entry; m.hi = entry + 24
    m.words = words; m.lifter = lifter.Lifter(base, 'c'); m.cache = {}; m.kinds = bytearray(len(words))
    m.stats = Counter(); m.patches = {}; m.pre_patches = {}; m.setjmps = set()
    m.fdes = []; m.fde_starts = []; m.local_branch_cache = {}
    m.jump_table = lambda *args: None
    assert m.computed_target(entry + 8, entry), hex(middle)
    labels, external, tables = m.scan(entry, entry + 24)
    assert tables == {entry + 8: 'all'} and not external
    assert all(ins.kind not in ('op', 'asm') for ins in m.cache.values())
    lifter.DYN_BASE = True
    blocks += m.emit_function(entry, entry + 24, labels, tables, {})
    # A real GPR overwrite still sends this branch to ordinary dispatch.
    m.words = (words[0], 0xaa0003e9, *words[2:]); m.cache = {}; m.kinds = bytearray(len(words))
    assert not m.computed_target(entry + 8, entry)

source = r'''
#include "cpu.h"
#include "simd.h"
#include <assert.h>
#include <stdio.h>
volatile int g_fnprof;
uint64_t totkgame_gb;
void fnprof_hit(uint64_t a,uint64_t b,uint64_t c) {(void)a;(void)b;(void)c;}
void rt_dispatch(Cpu *c) {(void)c;assert(!"unexpected external dispatch");}
void rt_udf(Cpu *c,uint64_t a) {(void)c;(void)a;assert(0);}
''' + '\n'.join(blocks) + r'''
int main(void) {
    void (*f[])(Cpu*)={f_7000000000,f_7000000100,f_7000000200};
    uint64_t bases[]={0x7000000000ull,0x100000000ull};
    for (int b=0;b<2;b++) for (int i=0;i<3;i++) {
        unsigned char stack[512]={0}; Cpu c={0};
        totkgame_gb=bases[b]; c.sp=(uintptr_t)stack;
        c.x[10]=bases[b]+i*0x100+16; c.x[30]=0x123456;
        f[i](&c);
        assert(c.x[0]==7 && c.x[30]==0x123456);
    }
    puts("FP stores/loads retain local table targets: six executions, relocated addresses, real GPR overwrite, branch-only cache");
}
'''
(out / 'test.c').write_text(source)
subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-I', str(root / 'runtime'),
                str(out / 'test.c'), '-lm', '-o', str(out / 'check')], check=True)
subprocess.run([str(out / 'check')], check=True)
