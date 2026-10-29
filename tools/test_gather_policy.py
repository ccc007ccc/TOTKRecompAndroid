#!/usr/bin/env python3
"""Exercise production gather defaults/overrides without creating a Vulkan device."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / '.work/totk-port/gather-policy-tests').resolve()
out.mkdir(parents=True, exist_ok=True)
execution = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = execution.index('static uint8_t gather_bias_bits(void)')
function = execution[start:execution.index('\n}\n', start) + 3]
source = out / 'gather-policy.c'
source.write_text('''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define __ANDROID__ 1
#define LOG(...) ((void)0)
static struct { struct { uint32_t vendorID; struct { uint32_t subTexelPrecisionBits; } limits; } props; } g_vk;
static int turnip, qualcomm;
bool vk_driver_is_turnip(void) { return turnip; }
bool vk_driver_is_qualcomm(void) { return qualcomm; }
''' + function + '''
int main(int argc, char **argv) {
    assert(argc == 6);
    g_vk.props.vendorID = strtoul(argv[1], NULL, 0);
    g_vk.props.limits.subTexelPrecisionBits = atoi(argv[2]);
    turnip = atoi(argv[3]); qualcomm = atoi(argv[4]);
    assert(gather_bias_bits() == atoi(argv[5]));
}
''')
binary = out / 'gather-policy'
subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                str(source), '-o', str(binary)], check=True)
# Each process starts with a fresh cached policy. Maxwell precision stays at 8
# regardless of the host limit; overrides must also retain the diagnostic off path.
cases = [
    (0x5143, 4, 0, 1, None, 8), (0x5143, 12, 1, 0, None, 8),
    (0, 4, 0, 1, None, 8), (0, 12, 1, 0, None, 8),
    (0x5143, 4, 0, 0, None, 8), (0x1002, 4, 0, 0, None, 8),
    (0x8086, 12, 0, 0, None, 8), (0x10de, 8, 0, 0, None, 0),
    (0x13b5, 8, 0, 0, None, 0), (0, 8, 0, 0, None, 0),
    (0x5143, 8, 0, 1, '0', 0), (0x5143, 8, 1, 0, '6', 6),
    (0x10de, 8, 0, 0, '8', 8), (0x5143, 8, 0, 1, '23', 23),
    (0x5143, 8, 0, 1, '24', 0), (0x5143, 8, 0, 1, '-1', 0),
]
for vendor, precision, turnip, qualcomm, override, expected in cases:
    env = os.environ.copy()
    env.pop('LARECOMP_GATHER_BIAS_BITS', None)
    if override is not None:
        env['LARECOMP_GATHER_BIAS_BITS'] = override
    subprocess.run([str(binary), str(vendor), str(precision), str(turnip), str(qualcomm), str(expected)],
                   env=env, check=True)
print(f'{len(cases)} production gather policy cases passed')
