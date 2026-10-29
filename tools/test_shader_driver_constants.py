#!/usr/bin/env python3
"""Check the production driver constant buffer, including point-size limits.

Run in dev after sourcing android/dev_env.sh. No game assets or GPU required.
"""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.work/totk-port/driver-constant-tests'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('static VkDescriptorBufferInfo cbuf_info(')
end = source.index('\n    if (k == 1)', start)
driver = source[start:end] + '\n    abort();\n}\n'
fixture = r'''
#include "runtime/nvn/vulkan/vk.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
VkG g_vk;
static struct {
    struct { uint64_t addr, size; } ubuf[6][16], sbuf[6][16];
} st;
enum { DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C };
static uint8_t captured[0x800];
static size_t captured_size;
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    (void)slot; assert(align == 256 && size <= sizeof captured);
    memcpy(captured, data, size); captured_size = size;
    return (VkSpan){.ptr = captured};
}
static VkDescriptorBufferInfo ubo_span(VkSpan span, uint64_t size) {
    assert(span.ptr == captured && size == captured_size);
    return (VkDescriptorBufferInfo){.range = size};
}
''' + driver + r'''
static uint32_t word(unsigned offset) {
    uint32_t v; assert(offset + sizeof v <= captured_size);
    memcpy(&v, captured + offset, sizeof v); return v;
}
static float point_limit(float hardware_max) {
    ShaderInfo si = {0}; si.cbuf_size[0] = 0x700;
    g_vk.props.limits.pointSizeRange[1] = hardware_max;
    cbuf_info(STAGE_VERTEX, &si, 0, 0, 0, 0);
    float v; memcpy(&v, captured + 0x610, sizeof v); return v;
}
int main(void) {
    st.sbuf[0][3].addr = UINT64_C(0x1234567890);
    st.sbuf[0][3].size = 0x1350;
    assert(point_limit(4096.0f) == 2047.9375f);
    assert(word(0x140) == 0x34567890 && word(0x144) == 0x12 && word(0x148) == 0x1350);
    assert(point_limit(256.0f) == 256.0f);
    assert(point_limit(1.0f) == 1.0f);
    // The vertex-only word must not overwrite fragment/compute driver state.
    ShaderInfo si = {0}; si.cbuf_size[0] = 0x700;
    st.sbuf[1][0].addr = 0x456; st.sbuf[1][0].size = 96;
    cbuf_info(STAGE_FRAGMENT, &si, 0, 0, 0, 0);
    assert(word(0x510) == 0x456 && word(0x518) == 96 && word(0x610) == 0);
    st.ubuf[5][0].addr = 0xabc; st.ubuf[5][0].size = 128;
    cbuf_info(STAGE_COMPUTE, &si, 0, 7, 11, 13);
    assert(word(0x200) == 7 && word(0x204) == 11 && word(0x208) == 13);
    assert(word(0x210) == 0xabc && word(0x218) == 128 && word(0x610) == 0);
    puts("PASS: point stamp limit, Vulkan device ceiling, storage tables and dispatch dimensions");
}
'''
unit = out / 'driver-constants.c'
unit.write_text(fixture)
binary = out / 'check'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined',
                '-I' + str(root), '-I' + str(root / 'android/deps/out/include'),
                str(unit), '-lm', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
