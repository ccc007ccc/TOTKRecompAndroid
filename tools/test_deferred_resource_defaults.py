"""Check the production game/GPU/driver default policy without a Vulkan device."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/device.c').read_text()


def helper(name):
    start = source.index('bool ' + name + '(')
    return source[start:source.index('\n}', start) + 2] + '\n'


unit = out / 'deferred-defaults.c'
unit.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int g_game_version = 143;
static bool turnip;
static struct { struct { char deviceName[256]; } props; } g_vk;
bool vk_driver_is_turnip(void) { return turnip; }
''' + helper('vk_adreno_8xx') + helper('vk_deferred_resources_default') + r'''
int main(void) {
    const char *names[] = {"Adreno (TM) 830v1", "Adreno830v2", "Adreno (TM) 750", "Mali-G715", ""};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        strcpy(g_vk.props.deviceName, names[i]);
        g_game_version = 143; turnip = true;
        assert(vk_deferred_resources_default() == (i < 2));
        turnip = false; assert(!vk_deferred_resources_default());
        turnip = true; g_game_version = 182; assert(!vk_deferred_resources_default());
    }
    puts("PASS: deferred resources default only for TotK 1.4.3 + Turnip + Adreno 8xx");
}
''')
binary = out / 'test-deferred-defaults'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=5)
