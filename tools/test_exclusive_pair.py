"""Execute emitted LDXP/STXP C on the dev host, including XZR/SP and W-register behavior."""
import os
import subprocess
from pathlib import Path

from lifter import Lifter

root = Path(__file__).resolve().parents[1]
work = root / ".work/totk-port/atomic-tests"
work.mkdir(parents=True, exist_ok=True)
instructions = {
    "load64": 0xC87FAA7F,   # ldaxp xzr, x10, [x19]
    "store64": 0xC82AA269,  # stlxp w10, x9, x8, [x19]
    "load32": 0x887F0BE1,   # ldxp w1, w2, [sp]
    "store32": 0x88240BE1,  # stxp w4, w1, w2, [sp]
}
lifter = Lifter(0x7000000000)
source = '#include "cpu.h"\n#include <assert.h>\n#include <stdio.h>\n'
for name, word in instructions.items():
    ins = lifter.lift(word, 0x7000000000)
    assert ins.kind == "op"
    source += f"static void {name}(Cpu *c) {{ {ins.code} }}\n"
source += r'''
int main(void) {
    __uint128_t memory __attribute__((aligned(16))) = ((__uint128_t)0x22 << 64) | 0x11;
    Cpu c = {0};
    c.x[19] = (uint64_t)(uintptr_t)&memory;
    c.x[31] = 0xbeef; c.nzcv = 0xa0000000;
    load64(&c);
    assert(c.x[10] == 0x22 && c.x[31] == 0xbeef && c.nzcv == 0xa0000000);
    c.x[9] = 0x33; c.x[8] = 0x44;
    store64(&c);
    assert(c.x[10] == 0 && memory == (((__uint128_t)0x44 << 64) | 0x33));
    store64(&c);
    assert(c.x[10] == 1);
    uint64_t small = 0x80000001ffffffffull;
    c.sp = (uint64_t)(uintptr_t)&small;
    load32(&c);
    assert(c.x[1] == 0xffffffff && c.x[2] == 0x80000001);
    c.x[1] = 0x100000003ull; c.x[2] = 0x200000004ull;
    store32(&c);
    assert(c.x[4] == 0 && small == 0x0000000400000003ull);
    assert(c.sp == (uint64_t)(uintptr_t)&small && c.nzcv == 0xa0000000);
    puts("paired exclusive lowering passed");
}
'''
(work / "lowering.c").write_text(source)
for src, name in ((root / "tools/test_exclusive_pair.c", "concurrency"), (work / "lowering.c", "lowering")):
    binary = work / name
    subprocess.run([os.environ.get("CC", "clang"), "-O1", "-mcx16", "-pthread", "-I", str(root / "runtime"),
                    str(src), "-latomic", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
