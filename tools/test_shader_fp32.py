#!/usr/bin/env python3
"""Check native FP32 lowering, retained binary64 mode and exact normal-range conversions (CPU only)."""
import argparse
import concurrent.futures
import ctypes as C
from pathlib import Path
import struct
import subprocess


def inspect_spirv(path, kind=None, production=False):
    raw = path.read_bytes()
    words = struct.unpack(f'<{len(raw) // 4}I', raw)
    assert words[0] == 0x07230203, path
    instructions, names, float32, values, stores, precise = [], {}, set(), {}, {}, set()
    i = 5
    while i < len(words):
        size, op = words[i] >> 16, words[i] & 65535
        assert size and i + size <= len(words), path
        inst = words[i:i + size]
        instructions.append(inst)
        assert not (op == 17 and inst[1] in (10, 11)), f'{path}: Float64/Int64 capability'
        assert not (op in (21, 22) and inst[2] == 64), f'{path}: 64-bit scalar type'
        if op == 5:
            names[inst[1]] = struct.pack(f'<{size - 2}I', *inst[2:]).split(b'\0')[0].decode()
        if op == 22 and inst[2] == 32:
            float32.add(inst[1])
        if op in (129, 133, 12):
            values[inst[2]] = inst
        if op == 62:
            stores[inst[1]] = inst[2]
        if op == 71 and inst[2] == 42:
            precise.add(inst[1])
        i += size
    if kind in ('add', 'mul', 'fma'):
        result = [stores[pointer] for pointer, name in names.items() if name == 'fr_']
        assert len(result) == 1, f'{path}: missing native result'
        inst = values[result[0]]
        assert inst[1] in float32, f'{path}: non-FP32 operation'
        # glslang decorates add/mul for precise, but not the explicit Fma builtin.
        # The production precision pass additionally decorates Fma.
        if kind != 'fma' or production:
            assert result[0] in precise, f'{path}: contractible operation'
        assert inst[0] & 65535 == {'add': 129, 'mul': 133, 'fma': 12}[kind], path
        if kind == 'fma':
            assert inst[4] == 50, f'{path}: DFMA lost fusion'
    return len(instructions)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=root / '.work/totk-port/shader-fp32/tests')
    parser.add_argument('--compiler', type=Path, help='Host production libshaderprepare.so; otherwise use glslc')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    generator = out / 'generate'
    subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined',
                    str(root / 'tools/test_shader_fp32.c'), '-lm', '-o', str(generator)], check=True)
    subprocess.run([str(generator), str(out)], check=True)
    cpu = out / 'conversions'
    subprocess.run(['clang++', '-O2', '-frounding-math', '-fsanitize=undefined',
                    str(out / 'conversions.cpp'), '-o', str(cpu)], check=True)
    subprocess.run([str(cpu)], check=True)
    paths = list(out.glob('*.vert'))
    assert len(paths) == 92, f'Expected 92 fixtures, found {len(paths)} (use a clean output directory)'
    if args.compiler:
        plugin = C.CDLL(str(args.compiler.resolve()))
        compile_shader = plugin.shader_prepare_compile
        compile_shader.argtypes = [C.c_char_p, C.c_int, C.c_uint, C.c_bool, C.POINTER(C.c_size_t), C.c_char_p, C.c_size_t]
        compile_shader.restype = C.c_void_p
        libc = C.CDLL(None)
        libc.free.argtypes = [C.c_void_p]

    def compile_one(path):
        binary = path.with_suffix('.vert.spv')
        if args.compiler:
            words, why = C.c_size_t(), C.create_string_buffer(8192)
            result = compile_shader(path.read_bytes(), 0, 0x00401000, False, C.byref(words), why, len(why))
            assert result, f'{path}: {why.value.decode()}'
            try:
                binary.write_bytes(C.string_at(result, words.value * 4))
            finally:
                libc.free(result)
        else:
            subprocess.run(['glslc', '--target-env=vulkan1.1', str(path), '-o', str(binary)],
                           capture_output=True, text=True, check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)],
                       capture_output=True, text=True, check=True)
        inspect_spirv(binary, path.stem.split('-')[0], bool(args.compiler))

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(compile_one, paths))
    print(f'{len(paths)} shaders validated; native FP32/FMA confirmed; no Float64/Int64; production={bool(args.compiler)}')


if __name__ == '__main__':
    main()
