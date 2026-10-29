"""Check the mutable-image view format lists and their Turnip/Adreno 8xx default without a Vulkan device."""
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)

# Expected texel sizes, independent of image_format_list.h: every uncompressed color format fmt_of() returns.
textures = (root / 'runtime/nvn/vulkan/textures.c').read_text()
body = textures[textures.index('static VkFmt fmt_of('):textures.index('bool renderer_format_is_depth(')]
produced = sorted(set(re.findall(r'VK_FORMAT_[A-Za-z0-9_]+', body)))


def texel_bytes(name):
    if re.search(r'_(BLOCK)$|^VK_FORMAT_[DS][0-9]|^VK_FORMAT_X8_D24', name):
        return 0  # compressed or depth/stencil: never mutable color views
    if re.match(r'VK_FORMAT_R32G32B32A32_', name): return 16
    if re.match(r'VK_FORMAT_(R16G16B16A16|R32G32)_', name): return 8
    if re.match(r'VK_FORMAT_(R32|R16G16|R8G8B8A8|B8G8R8A8)_', name) or name.endswith('_PACK32'): return 4
    if re.match(r'VK_FORMAT_(R16|R8G8)_', name) or name.endswith('_PACK16'): return 2
    if re.match(r'VK_FORMAT_R8_', name): return 1
    raise AssertionError('unclassified format ' + name)


color = [n for n in produced if texel_bytes(n)]
assert 'VK_FORMAT_E5B9G9R9_UFLOAT_PACK32' in color and 'VK_FORMAT_B10G11R11_UFLOAT_PACK32' in color, produced
names = '\n'.join('    {"%s", %s},' % (n, n) for n in color)
unit = out / 'image-format-lists.c'
unit.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "image_format_list.h"
static const struct { const char *name; VkFormat f; } formats[] = {
''' + names + r'''
};
static const char *name_of(VkFormat f) {
    for (unsigned i = 0; i < sizeof formats / sizeof *formats; i++) if (formats[i].f == f) return formats[i].name;
    return NULL;
}
int main(void) {
    for (unsigned i = 0; i < sizeof formats / sizeof *formats; i++) {
        const VkFormat *list;
        uint32_t n = vk_image_view_formats(formats[i].f, &list);
        printf("%s", formats[i].name);
        for (uint32_t k = 0; k < n; k++) {
            const char *m = name_of(list[k]);
            printf(" %s", m ? m : "UNKNOWN");
            for (uint32_t j = 0; j < k; j++) assert(list[j] != list[k]);
        }
        printf("\n");
    }
}
''')
binary = out / 'test-image-format-lists'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined', '-I',
                str(root / 'runtime/nvn/vulkan'), str(unit), '-o', str(binary)], check=True)
lists = {line.split()[0]: line.split()[1:] for line in
         subprocess.run([str(binary)], check=True, timeout=5, capture_output=True, text=True).stdout.splitlines()}
for name in color:
    members = lists[name]
    if name == 'VK_FORMAT_E5B9G9R9_UFLOAT_PACK32':
        assert members == [], members  # never UBWC: such images keep plain mutability
        continue
    assert name in members, (name, members)
    assert 'UNKNOWN' not in members and 'VK_FORMAT_E5B9G9R9_UFLOAT_PACK32' not in members, (name, members)
    same = sorted(n for n in color if texel_bytes(n) == texel_bytes(name) and n != 'VK_FORMAT_E5B9G9R9_UFLOAT_PACK32')
    assert sorted(members) == same, (name, members, same)  # the whole class: NVN views may use any of it

# Default policy: Turnip on Adreno 8xx only.
device = (root / 'runtime/nvn/vulkan/device.c').read_text()


def helper(name):
    start = device.index('bool ' + name + '(')
    return device[start:device.index('\n}', start) + 2] + '\n'


policy = out / 'image-format-list-default.c'
policy.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool turnip;
static struct { struct { char deviceName[256]; } props; } g_vk;
bool vk_driver_is_turnip(void) { return turnip; }
''' + helper('vk_adreno_8xx') + helper('vk_image_format_lists_default') + r'''
int main(void) {
    const char *names[] = {"Adreno (TM) 830v1", "Adreno (TM) 810", "Adreno (TM) 750", "Adreno (TM) 740", "Mali-G715", ""};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        strcpy(g_vk.props.deviceName, names[i]);
        turnip = true; assert(vk_image_format_lists_default() == (i < 2));
        turnip = false; assert(!vk_image_format_lists_default());
    }
    puts("PASS: view format lists by default only for Turnip + Adreno 8xx");
}
''')
policy_binary = out / 'test-image-format-list-default'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined', str(policy), '-o',
                str(policy_binary)], check=True)
subprocess.run([str(policy_binary)], check=True, timeout=5)
print('PASS: %d color formats, each listed with its whole UBWC-capable compatibility class' % len(color))
