#!/usr/bin/env python3
"""Exercise production compiler argument translation and resume identities; optionally check pinned Zig."""
import argparse
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/gamebuild-flags-tests"
OUT.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default="cc")
parser.add_argument("--zig", type=Path)
args = parser.parse_args()
source = (ROOT / "runtime/gamebuild.c").read_text()
arguments = source[source.index("typedef struct { char **v;"):source.index("// ------------------------------------------------------------------ the tools")]
start = source.index("static void add_cflags(")
cflags = source[start:source.index("#ifdef __ANDROID__", start)]
start = source.index("static void build_stamp(")
stamp = source[start:source.index("static bool unit_done(", start)]
(OUT / "driver.c").write_text('''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SEP "/"
typedef struct { bool zig; } Tools;
''' + arguments + cflags + stamp + '''
int main(int argc, char **argv) {
    char stamp[512];
    build_stamp(argv[1], stamp, sizeof stamp);
    puts(stamp);
    Args a = {0};
    Tools t = {.zig = atoi(argv[2])};
    add_cflags(&a, &t);
    for (int i = 0; i < a.n; i++) puts(a.v[i]);
    args_free(&a);
    return argc < 3;
}
''')
game = OUT / "game"
(game / "exefs").mkdir(parents=True, exist_ok=True)
modules = ("main", "sdk", "subsdk0", "rtld")
for i, module in enumerate(modules):
    (game / "exefs" / module).write_bytes(bytes(0x40) + bytes([i + 1]) * 32)
flags = "-target aarch64-linux-musl -O3 -flto=thin -fno-fast-math -march=armv8.7-a+crypto+fp16 -mtune=oryon-1 -fPIC -fno-sanitize=undefined -fno-stack-protector -g0 -mllvm -disable-licm-promotion"
shared = "-target aarch64-linux-musl -shared -nostdlib -O3 -flto=thin -Wl,-z,max-page-size=16384"


def compile_driver(name, cflags=flags, link_flags=shared):
    output = OUT / name
    subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    '-DGAMELIB_ID="0123456789abcdef"', f'-DGAMELIB_CFLAGS="{cflags}"',
                    f'-DGAMELIB_SHARED="{link_flags}"', str(OUT / "driver.c"), "-o", str(output)], check=True)
    return output


def run(driver, zig=True):
    return subprocess.check_output([str(driver), str(game), str(int(zig))], text=True).splitlines()


driver = compile_driver("driver")
baseline, *translated = run(driver)
assert run(driver, False)[1:] == flags.split()
assert "-mcpu=generic+v8_7a+crypto+fullfp16" in translated
at = translated.index("-tune-cpu")
assert translated[at - 1:at + 3] == ["-Xclang", "-tune-cpu", "-Xclang", "oryon-1"]
assert not any(flag.startswith(("-march=", "-mtune=")) for flag in translated)
for module in modules:
    path = game / "exefs" / module
    original = path.read_bytes()
    path.write_bytes(original[:-1] + bytes([original[-1] ^ 255]))
    assert run(driver)[0] != baseline, f"Trailing bytes of {module}'s build ID were ignored"
    path.write_bytes(original)
assert run(compile_driver("compile-changed", flags + " -fno-omit-frame-pointer"))[0] != baseline
assert run(compile_driver("link-changed", link_flags=shared + " -Wl,--build-id"))[0] != baseline

if args.zig:
    probe = OUT / "probe.c"
    probe.write_text("int profile_probe(int a) { return a * 7 + 3; }\n")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(OUT / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(OUT / "zig-local"))
    obj, library = OUT / "probe.o", OUT / "probe.so"
    compiled = subprocess.run([str(args.zig), "cc", *translated, "-v", "-c", str(probe), "-o", str(obj)],
                              capture_output=True, text=True, env=env, check=True)
    assert "-tune-cpu oryon-1" in compiled.stderr and "-target-feature +v8.7a" in compiled.stderr
    linked = subprocess.run([str(args.zig), "cc", *shared.split(), "-v", str(obj), "-o", str(library)],
                            capture_output=True, text=True, env=env, check=True)
    assert "--lto-O3" in linked.stderr and library.stat().st_size > 0
    (OUT / "zig-compile.log").write_text(compiled.stderr)
    (OUT / "zig-link.log").write_text(linked.stderr)
print("Production NDK/Zig arguments, O3 LTO linking and all four full module IDs passed")
