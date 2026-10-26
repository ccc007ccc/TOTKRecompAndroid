"""Execute generated C for an ADR local return crossing conditional branches."""
import struct
import subprocess
from pathlib import Path

import lifter
from gen import Module

root = Path(__file__).resolve().parents[1]
work = root / ".work/totk-port/branch-tests"
work.mkdir(parents=True, exist_ok=True)
base = 0x7000000000

# Synthetic NSO, no proprietary bytes: entry 0x100, local return 0x120.
data = bytearray(0x300)
data[:4] = b"NSO0"
struct.pack_into("<III", data, 0x10, 0x100, 0, 0x200)
struct.pack_into("<III", data, 0x20, 0x300, 0x200, 0)
struct.pack_into("<III", data, 0x30, 0x300, 0x200, 0)
struct.pack_into("<I", data, 0x104, 0x20)
data[0x120:0x124] = b"MOD0"
struct.pack_into("<i", data, 0x124, 0x60)  # dynamic table at 0x80
struct.pack_into("<i", data, 0x130, -0x20)  # no .eh_frame_hdr
words = [
    0x10000113,  # adr x19, .+0x20
    0xB4000060,  # cbz x0, .+0xc
    0xD28000E1,  # mov x1, #7
    0x14000002,  # b .+8
    0xD2800121,  # mov x1, #9
    0xD503201F,  # nop
    0xD61F0260,  # br x19
    0xD65F03C0,  # ret
    0x91001420,  # add x0, x1, #5
    0xD65F03C0,  # ret
]
struct.pack_into("<10I", data, 0x200, *words)
image = work / "synthetic.nso"
image.write_bytes(data)
lifter.DYN_BASE = True
module = Module("test", str(image), base, "asm")
entry, end = base + 0x100, base + 0x128
labels, _, tables = module.scan(entry, end)
body = module.emit_function(entry, end, labels, tables, {})
source = r'''
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
volatile int g_fnprof;
uint64_t totkgame_gb;
static int dispatched;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t c) {(void)a;(void)b;(void)c;}
void rt_dispatch(Cpu *c) {(void)c;dispatched++;}
void rt_udf(Cpu *c, uint64_t a) {(void)c;(void)a;assert(0);}
'''
source += "\n".join(body)
source += r'''
int main(void) {
    uint64_t bases[] = {0x7000000000ull, 0x100000000ull};
    for (int i = 0; i < 2; i++) {
        totkgame_gb = bases[i];
        for (int x = 0; x < 2; x++) {
            Cpu c = {0}; c.x[0] = x; c.x[30] = 0x12345678;
            f_7000000100(&c);
            assert(c.x[0] == (x ? 12 : 14));
            assert(c.x[30] == 0x12345678 && !dispatched);
        }
    }
    puts("local indirect branches passed (conditional paths and relocated window)");
}
'''
(work / "branch.c").write_text(source)
binary = work / "branch"
subprocess.run(["clang", "-O1", "-I", str(root / "runtime"), str(work / "branch.c"), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
