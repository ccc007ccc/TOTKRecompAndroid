"""Exercise actual uniform preflight/binding helpers with a delayed mock GPU."""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
execution = (root / 'runtime/nvn/vulkan/exec.c').read_text()


def helper(name):
    match = re.search(r'^(?:static )?\w+ ' + name + r'\([^\n]*\) \{', execution, re.M)
    assert match, name
    return execution[match.start():execution.index('\n}', match.start()) + 2] + '\n'


fixture = (root / 'tools/test_buffer_copy.c').read_text().split('int main(', 1)[0]
inline_start = execution.index('enum { UBO_INLINE_SLOTS')
inline_code = execution[inline_start:execution.index('\n#endif', inline_start)]
unit = out / 'buffer-copy-uniforms.c'
unit.write_text(fixture + r'''
#include <stdatomic.h>
#undef HEAP_BASE
#define HEAP_BASE 0x10000ull
typedef struct { uint64_t addr, size; } Range;
static struct { NvnProgram *program; Range ubuf[6][18], sbuf[6][16]; } st;
VkG g_vk;
static VkBuffer gmem_buf = (VkBuffer)1;
static uint64_t gmem_size = sizeof memory;
static bool gmem_placed = true, snapshot, g_debug_frame, trace_copy_bytes;
static bool submit_snapshot_enabled(void) { return snapshot; }
static bool vk_diag_active(void) { return false; }
static bool ct_on(void) { return false; }
uint64_t tegra_total_size(const TexLayout *layout) { return layout->width; }
static int naliases;
static NvnTexture *alias_read(NvnTexture *t) { return t; }
static NvnTexture *shader_texture_object(int stage, ShaderTexture *texture) {
    Range r = st.ubuf[stage][texture->bindless_cbuf - 3];
    // A descriptor lookup really reads the handle on the CPU.
    assert(memory[r.addr - HEAP_BASE + texture->bindless_offset] == 0x51);
    return NULL;
}
''' + inline_code + helper('ubo_shared') + helper('copy_uniform_begin') +
    helper('copy_uniform_cpu_diagnostic') + helper('copy_stage_inputs') +
    helper('copy_texture_input') + helper('copy_stage_textures') + r'''
int main(void) {
    setenv("LARECOMP_VK_ASYNC_BUFFER_COPY", "1", 1);
    memset(memory, 0x51, 32);
    g_vk.props.limits.minUniformBufferOffsetAlignment = 16;
    g_vk.props.limits.maxUniformBufferRange = 65536;
    NvnProgram program = {0}; st.program = &program;
    // GPU writes just the tail; the full uniform is read directly. Fragment
    // slot 0's CPU prefix is unrelated and must not force a tail readback.
    st.ubuf[1][0] = (Range){0x10100, 32};
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    copy_uniform_begin(); copy_stage_inputs(1);
    assert(!waits && copy_ubo_mask[1] == 1 && !memory[0x110]);
    // A recording-thread mark after preflight must not change this draw's
    // binding into an unsafe CPU upload. The later CPU write still waits.
    vk_ubo_inline_mark(0x10100, 4);
    VkDescriptorBufferInfo d;
    assert(!ubo_shared(0x10100, 32, &d));
    assert(copy_uniform_info(1, 0, 32, &d) && d.offset == 0x100 && d.range == 32);
    assert(!waits);
    vk_buffer_copy_cpu_write(0x10100, 4); assert(waits == 1);
    copy_uniform_begin(); assert(!copy_uniform_info(1, 0, 32, &d));
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    copy_stage_inputs(1); assert(waits == 2 && !copy_ubo_mask[1]);
    memset(ubo_inline, 0, sizeof ubo_inline);
    // Bindless handles are CPU consumers, including words inside the copied
    // tail. Read their exact word before looking up a texture descriptor.
    memset(memory + 0x110, 0, 16);
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    copy_uniform_begin(); copy_stage_inputs(1); assert(waits == 2);
    ShaderInfo shader = {.ntex = 1};
    shader.tex[0] = (ShaderTexture){.bindless = true, .bindless_cbuf = 3, .bindless_offset = 16};
    copy_stage_textures(1, &shader); assert(waits == 3);
    // Waiting for a texture word can retire the copy without losing the
    // reserved direct binding's protection in the next submission.
    assert(copy_uniform_info(1, 0, 16, &d) && d.range == 16);
    vk_buffer_copy_cpu_write(0x10100, 4); assert(waits == 4);
    // CPU prefix, diagnostics, snapshots, inline buffers, and unsupported
    // placements retain preflight. No wait may be deferred until binding.
    assert(vk_buffer_copy_async(0x10000, 0x10100, 16));
    copy_uniform_begin(); copy_stage_inputs(1); assert(waits == 5 && !copy_ubo_mask[1]);
    for (unsigned mode = 0; mode < 5; ++mode) {
        g_debug_frame = mode == 0; snapshot = mode == 1;
        gmem_placed = mode != 2;
        st.ubuf[1][0] = (Range){mode == 3 ? 0x10104 : 0x10100, 32};
        if (mode == 4) vk_ubo_inline_mark(0x10110, 4);
        assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
        copy_uniform_begin(); copy_stage_inputs(1);
        assert(waits == 6 + mode && !copy_ubo_mask[1]);
    }
    memset(ubo_inline, 0, sizeof ubo_inline);
    assert(!ubo_shared(UINT64_MAX - 7, 32, &d));
    assert(!ubo_shared(HEAP_BASE + sizeof memory - 16, 32, &d));
    assert(!ubo_shared(HEAP_BASE, 0, &d));
    // Widescreen vertex slots and storage buffers keep CPU visibility.
    st.ubuf[0][5] = (Range){0x10100, 32};
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    copy_uniform_begin(); copy_stage_inputs(0); assert(waits == 11 && !copy_ubo_mask[0]);
    st.sbuf[5][0] = (Range){0x10100, 32};
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    copy_uniform_begin(); copy_stage_inputs(5); assert(waits == 12);
    puts("PASS: direct uniform preflight, late inline mark, bindless CPU reads, actual binding seq, snapshots/diagnostics and unsupported-range fallback");
}
''')
binary = out / 'test-buffer-copy-uniforms'
subprocess.run([os.environ.get('CC', 'clang'), '-D__ANDROID__', '-O1', '-g', '-fsanitize=address,undefined',
    '-ffunction-sections', '-fdata-sections', '-I' + str(root / 'tools'),
    '-I' + str(root / 'android/deps/out/include'), '-I' + str(root / 'android/deps/out/volk'),
    str(unit), '-Wl,--gc-sections', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=10)
