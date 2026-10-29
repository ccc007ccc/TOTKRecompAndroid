#!/usr/bin/env python3
"""Compare derived interfaces with independent GLSL compilation after IR normalization.

This is an interface/bitcast test, not a GPU arithmetic or gameplay test.
Run inside dev: python3 tools/test_shader_interfaces.py PLUGIN OUTPUT_DIRECTORY
"""
import ctypes as c
import itertools
from pathlib import Path
import re
import subprocess
import sys


class Compiler:
    def __init__(self, path):
        self.plugin = c.CDLL(str(Path(path).resolve()))
        self.libc = c.CDLL(None)
        self.libc.free.argtypes = [c.c_void_p]
        self.compile = self.plugin.shader_prepare_compile
        self.compile.argtypes = [c.c_char_p, c.c_int, c.c_uint, c.c_bool,
                                 c.POINTER(c.c_size_t), c.c_char_p, c.c_size_t]
        self.compile.restype = c.c_void_p
        self.derive = self.plugin.shader_prepare_derive
        self.derive.argtypes = [c.c_void_p, c.c_size_t, c.c_int, c.c_void_p,
                                c.c_uint, c.POINTER(c.c_size_t), c.c_char_p, c.c_size_t]
        self.derive.restype = c.c_void_p

    def call(self, fn, *args):
        n, why = c.c_size_t(), c.create_string_buffer(8192)
        ptr = fn(*args, c.byref(n), why, len(why))
        if not ptr:
            raise RuntimeError(why.value.decode())
        try:
            return c.string_at(ptr, n.value * 4)
        finally:
            self.libc.free(ptr)


def source(stage, kinds):
    types = ['vec4', 'ivec4', 'uvec4']
    decl = '\n'.join(f'layout(location={i}) {"in" if stage == 0 else "out"} {types[k]} a{i};'
                     for i, k in enumerate(kinds))
    if stage == 0:
        loads = [f'floatBitsToUint(a{i})' if k == 0 else f'uvec4(a{i})'
                 for i, k in enumerate(kinds)]
        body = f'''uvec4 r = {loads[0]} ^ {loads[1]};
  result = r; gl_Position = vec4(uintBitsToFloat(r.x), 0, 0, 1);'''
        return f'#version 450\n{decl}\nlayout(location=0) flat out uvec4 result;\nvoid main(){{{body}}}'
    stores = [f'uintBitsToFloat(r)' if k == 0 else f'{types[k]}(r)' for k in kinds]
    scalar = ['uintBitsToFloat', 'int', 'uint'][kinds[1]]
    components = ' '.join(f'a1.{comp} = {scalar}(r.{comp});' for comp in 'xyzw')
    tint_decl = 'layout(set=1,binding=19,std140) uniform Tint {vec4 ltint[2];};' if kinds[0] == 0 else ''
    tint = '''if (ltint[0].w > 0.0) { vec3 g = a0.rgb; float l = dot(g, vec3(0.299,0.587,0.114));
    a0.rgb = mix(g, ltint[0].rgb * pow(l,ltint[1].w) + ltint[1].rgb*l, ltint[0].w); }''' if kinds[0] == 0 else ''
    return f'''#version 450
{decl}
layout(location=0) flat in uvec4 r;
{tint_decl}
void main(){{a0 = {stores[0]}; {components} {tint}}}
'''


def normalized(path):
    out = path.with_suffix('.opt.spv')
    subprocess.run(['spirv-opt', '-O', '--strip-debug', '--canonicalize-ids',
                    '--strip-debug', str(path), '-o', str(out)], check=True)
    text = subprocess.check_output(['spirv-dis', '--raw-id', str(out)], text=True)
    # Module generator/bound metadata differ; executable declarations/instructions must agree.
    lines = [line.strip() for line in text.splitlines() if not line.startswith(';')]
    function = next(i for i, line in enumerate(lines) if ' = OpFunction ' in line)
    definitions = dict(re.findall(r'^(%\d+) = (.*)$', '\n'.join(lines[:function]), re.M))
    # These instructions have no side effects. Inline their expression trees so
    # scheduling an address before/after its stored value does not affect equality.
    pure = dict(re.findall(r'^(%\d+) = (Op(?:Bitcast|AccessChain|InBoundsAccessChain) .*)$',
                           '\n'.join(lines[function:]), re.M))
    definitions.update(pure)
    lines = [line for line in lines if not (match := re.match(r'(%\d+) = ', line)) or match[1] not in pure]
    names = {match[1]: f'%f{i}' for i, line in enumerate(lines[function:])
             if (match := re.match(r'(%\d+) = ', line))}

    def replace(match):
        identifier = match[0]
        if identifier not in names:
            value = definitions[identifier]
            # Interface identity includes its location, not just its numeric type.
            if value.startswith('OpVariable '):
                value += '[' + ','.join(sorted(line.split(' ', 2)[2] for line in lines[:function]
                    if line.startswith(f'OpDecorate {identifier} '))) + ']'
            names[identifier] = '{' + re.sub(r'%\d+', replace, value) + '}'
        return names[identifier]

    lines = [re.sub(r'%\d+', replace, line) for line in lines]
    return '\n'.join(sorted(lines[:function]) + lines[function:])


def main():
    compiler = Compiler(sys.argv[1])
    directory = Path(sys.argv[2]); directory.mkdir(parents=True, exist_ok=True)
    checked = 0
    for api_minor, optimized, stage in itertools.product([1, 3], [False, True], [0, 1]):
        api = (1 << 22) | (api_minor << 12)
        base = compiler.call(compiler.compile, source(stage, [0, 0]).encode(), stage, api, optimized)
        for kinds in itertools.product(range(3), repeat=2):
            name = f'{stage}-{api_minor}-{int(optimized)}-{kinds[0]}{kinds[1]}'
            derived = compiler.call(compiler.derive, base, len(base)//4, stage,
                                    (c.c_uint8 * 32)(*kinds), api)
            direct = compiler.call(compiler.compile, source(stage, kinds).encode(), stage, api, optimized)
            paths = [directory / (name + suffix + '.spv') for suffix in ['-derived', '-direct']]
            for path, data in zip(paths, [derived, direct]):
                path.write_bytes(data)
                subprocess.run(['spirv-val', '--target-env', f'vulkan1.{api_minor}', str(path)], check=True)
            a, b = map(normalized, paths)
            if a != b:
                paths[0].with_suffix('.txt').write_text(a)
                paths[1].with_suffix('.txt').write_text(b)
                raise AssertionError(f'normalized direct/derived IR differs: {name}')
            checked += 1
    print(f'PASS: {checked} direct/derived modules have identical normalized IR; Vulkan 1.1/1.3, both optimization modes')


if __name__ == '__main__':
    main()
