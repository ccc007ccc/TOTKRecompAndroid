#!/usr/bin/env python3
"""Hot / cold game code build (runtime/gamebuild.c, Android) on the PC: the production load_hot / mark_cold on
synthetic generated files (Linux / WSL cc), and tools/hot_functions.py's address conversion and game versions.
Usage: test_gamebuild_hot.py [--cc cc]"""
import argparse
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default='cc')
args = p.parse_args()
out = root / 'build' / 'gamebuild-hot-tests'
out.mkdir(parents=True, exist_ok=True)
checks = 0


def hot_functions(*a, ok=True):
    r = subprocess.run([sys.executable, str(root / 'tools/hot_functions.py'), *map(str, a)], capture_output=True, text=True)
    assert (r.returncode == 0) == ok, (r.returncode, r.stdout, r.stderr)
    return r


def entries(path):
    return [l for l in path.read_text().splitlines() if not l.startswith('#')]


# 1. tools/hot_functions.py: a phone session (guest window at 0x100000000) and a desktop one of 1.8.2, merged
phone = out / 'phone.txt'
phone.write_text('# game_version 182\n# main_base 100000000\n100005350 7\n104001000 2\n100000010 1\n200000000 5\n')
desk = out / 'desk.txt'
desk.write_text('# game_version 182\n7000005350 3\n7000009000 1\n')
hot = out / 'hot.txt'
hot_functions(phone, desk, '-o', hot)
assert entries(hot) == ['7000000010', '7000005350', '7000009000', '7004001000'], entries(hot)
checks += 1
hot_functions(phone, desk, '-o', hot, '--min-count', '3')
assert entries(hot) == ['7000005350'], entries(hot)  # (7 + 3 entries: the same function in both sessions)
checks += 1
mod = out / 'modules.txt'  # ("<module> <offset>", no version: --version)
mod.write_text('main 30\nsdk 1c0\nmain 5350\n')
hot_functions(mod, desk, '-o', hot, '--version', '182')
assert entries(hot) == ['7000000030', '7000005350', '7000009000', '70040001c0'], entries(hot)
checks += 1
old = out / 'old.txt'  # (another game version: refused)
old.write_text('# game_version 100\n7000001000 4\n')
r = hot_functions(phone, old, '-o', hot, ok=False)
assert 'one version per list' in r.stderr, r.stderr
checks += 1
hot_functions(mod, '-o', hot, ok=False)  # (no version named at all)
checks += 1

# 2. mark_cold, the production code, on generated-looking files
source = (root / 'runtime/gamebuild.c').read_text()
start = source.index('#define HEAVY_LABELS')
body = source[start:source.index('// The hot list for the game in game_dir')]
(out / 'mark.c').write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef GAMELIB_PERFORMANCE
#define GAMELIB_PERFORMANCE 0
#endif
''' + body + r'''
int main(int argc, char **argv) {
    load_hot(argv[1]);
    int o0 = 0, alone = 0;
    for (int i = 2; i < argc; i++) {
        bool cold, heavy;
        if (!mark_cold(argv[i], &cold, &heavy)) return 1;
        o0 += cold;
        alone += heavy;
    }
    printf("%llu %llu %llu %zu %d %llu %d\n", g_cold_fns, g_all_fns, g_heavy_fns, g_nhot, o0, g_cold_files, alone);
    return 0;
}
''')
OPT = '__attribute__((optnone, noinline)) '
chunk = ('#include "cpu.h"\n'
         'void f_7000000010(Cpu *c);\n'
         'void f_7000005350(Cpu *c);\n'
         'void f_7000009000(Cpu *c) {\n    f_7000000010(c);\n}\n'
         '\n'
         'void f_7000000010(Cpu *c) {\n    /* hot */\n}\n'
         'void f_70000aaaa0(Cpu *c) {\n    // void f_7000000020(Cpu *c) { (not at a line start)\n}\n')
expected = chunk.replace('void f_7000009000(Cpu *c) {', OPT + 'void f_7000009000(Cpu *c) {') \
                .replace('void f_70000aaaa0(Cpu *c) {', OPT + 'void f_70000aaaa0(Cpu *c) {')
# a giant hot function (a jump table's: a label per instruction; at the file's first line) and a hot one with labels
# below the limit: nothing cold, so the file stays as it is - it is only to be compiled at -O0
giant = ('void f_7000005350(Cpu *c) {\n' + ''.join(f'L_{0x7000005350 + 4 * i:x}:;\n    c->x[0]++;\n' for i in range(15001)) +
         '}\nvoid f_7000000010(Cpu *c) {\n' + ''.join(f'L_{0x7000000010 + 4 * i:x}:;\n' for i in range(100)) + '}\n')
giant_expected = giant
# a file of cold functions only: compiled at -O0 as it is (not marked)
cold = '#include "cpu.h"\nvoid f_7000020000(Cpu *c) {\n}\nvoid f_7000020010(Cpu *c) {\n}\n'
(out / 'list.txt').write_text(  # (both forms; a comment longer than the line buffer, with hex-looking text)
    '# test ' + 'a' * 57 + ' 7000009000 abc\nmain 10\n7000005350\nsdk 0\n')
src, gsrc, csrc = out / 'chunk.c', out / 'giant.c', out / 'cold.c'
src.write_bytes(chunk.encode())
gsrc.write_bytes(giant.encode())
csrc.write_bytes(cold.encode())
cmtime = csrc.stat().st_mtime_ns
subprocess.run([args.cc, '-O1', '-std=gnu11', '-Wall', '-Werror', str(out / 'mark.c'), '-o', str(out / 'mark')], check=True)
r = subprocess.run([str(out / 'mark'), str(out / 'list.txt'), str(src), str(gsrc), str(csrc)], check=True, capture_output=True,
                   text=True)
# (4 cold of 7 definitions, 1 giant; 3 hot loaded; 2 files at -O0: the giant one and the all-cold one)
assert r.stdout.split() == ['4', '7', '1', '3', '2', '1', '1'], r.stdout
checks += 1
assert src.read_bytes() == expected.encode(), src.read_text()
checks += 1
assert gsrc.read_bytes() == giant_expected.encode(), gsrc.read_text()[:300]
checks += 1
assert csrc.read_bytes() == cold.encode() and csrc.stat().st_mtime_ns == cmtime, 'the all-cold file was rewritten'
checks += 1
assert not any(out.glob('*.c.cold')), 'temporary file left behind'
checks += 1

# 3. no hot list: nothing is marked and no file rewritten, but the giant one still goes to -O0
src.write_bytes(chunk.encode())
gsrc.write_bytes(giant.encode())
mtime, gmtime = src.stat().st_mtime_ns, gsrc.stat().st_mtime_ns
r = subprocess.run([str(out / 'mark'), str(out / 'missing-list.txt'), str(src), str(gsrc), str(csrc)], check=True,
                   capture_output=True, text=True)
assert r.stdout.split() == ['0', '7', '1', '0', '1', '0', '1'], r.stdout  # (no list: only the giant file at -O0, alone)
checks += 1
assert src.read_bytes() == chunk.encode() and src.stat().st_mtime_ns == mtime, 'a file with nothing to mark was rewritten'
checks += 1
assert gsrc.read_bytes() == giant.encode() and gsrc.stat().st_mtime_ns == gmtime, 'the giant file was rewritten'
checks += 1
large = out / 'large.c'
large.write_text('void f_7000005350(Cpu *c) {\n' + '    c->x[0]++;\n' * 60001 + '}\n')
r = subprocess.run([str(out / 'mark'), str(out / 'missing-list.txt'), str(large)],
                   check=True, capture_output=True, text=True)
assert r.stdout.split() == ['0', '1', '1', '0', '1', '0', '1'], r.stdout
checks += 1

# 4. Performance profiles keep all definitions optimized, even when a hot list is present.
# Giant functions still run alone to limit peak memory; none of the input files is rewritten.
subprocess.run([args.cc, '-DGAMELIB_PERFORMANCE=1', '-O3', '-std=gnu11', '-Wall', '-Werror',
                str(out / 'mark.c'), '-o', str(out / 'mark-performance')], check=True)
src.write_bytes(chunk.encode())
gsrc.write_bytes(giant.encode())
files = (src, gsrc, csrc, large)
before = [(f.read_bytes(), f.stat().st_mtime_ns) for f in files]
for hot_list, count in ((out / 'list.txt', '3'), (out / 'missing-list.txt', '0')):
    r = subprocess.run([str(out / 'mark-performance'), str(hot_list), *map(str, files)],
                       check=True, capture_output=True, text=True)
    assert r.stdout.split() == ['0', '8', '2', count, '0', '0', '2'], r.stdout
    checks += 1
    assert [(f.read_bytes(), f.stat().st_mtime_ns) for f in files] == before, 'performance code was rewritten'
    checks += 1
assert not any(out.glob('*.c.cold')), 'performance build left a cold source file'
checks += 1
print(f'{checks} hot/cold build checks passed (production C and tools/hot_functions.py)')
