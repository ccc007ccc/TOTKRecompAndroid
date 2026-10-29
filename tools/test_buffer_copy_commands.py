"""Exercise the production command preflight against the delayed GPU copy fixture."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
execution = (root / 'runtime/nvn/vulkan/exec.c').read_text()

def helper(name):
    start = execution.index('static void ' + name + '(')
    return execution[start:execution.index('\n}', start) + 2]

fixture = (root / 'tools/test_buffer_copy.c').read_text().split('int main(', 1)[0]
unit = out / 'buffer-copy-commands.c'
unit.write_text(fixture + r'''
static struct { struct { int ncolor; NvnTexture *color[8], *depth; } rt; } st;
uint64_t tegra_total_size(const TexLayout *layout) { return layout->width; }
''' + helper('copy_texture_input') + helper('copy_command_inputs') + r'''
int main(void) {
    setenv("LARECOMP_VK_ASYNC_BUFFER_COPY", "1", 1);
    memset(memory, 0x71, 32);
    CmdRec *command = calloc(1, sizeof(CmdRec) + sizeof(uint64_t));
    assert(command);
    unsigned before = waits;
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    command->op = CMD_SET_RENDER_TARGETS;
    copy_command_inputs(command);
    assert(waits == before && vk_buffer_copy_pending() && !memory[0x100]);
    NvnTexture color = {.addr = 0x11000, .layout = {.width = 32}};
    NvnTexture depth = {.addr = 0x11200, .layout = {.width = 32}};
    st.rt.ncolor = 1; st.rt.color[0] = &color; st.rt.depth = &depth;
    const unsigned clears[] = {CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE};
    for (unsigned i = 0; i < 3; ++i) {
        command->op = clears[i];
        *(uint64_t *)command->extra = (uintptr_t)&color;
        copy_command_inputs(command);
        assert(waits == before && vk_buffer_copy_pending());
    }
    // An unrelated counter must not flush; its actual 16-byte write does.
    command->op = CMD_REPORT_COUNTER; command->x[1] = 0x11000;
    copy_command_inputs(command); assert(waits == before);
    command->x[1] = 0x10110;
    copy_command_inputs(command); assert(waits == ++before && memory[0x100] == 0x71);
    // The timestamp read just before an adjacent counter also depends on the copy.
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    command->x[1] = 0x10120;
    copy_command_inputs(command); assert(waits == ++before);
    // Counter writes must protect the source too; a read of it alone would not.
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    command->x[1] = 0x10000;
    copy_command_inputs(command); assert(waits == ++before);
    for (unsigned i = 0; i < 3; ++i) {
        assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
        command->op = clears[i];
        color.addr = 0x1011c; // partial overlap at the destination's end
        copy_command_inputs(command); assert(waits == ++before);
    }
    color.addr = 0x11000; depth.addr = 0x1011c;
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    command->op = CMD_CLEAR_COLOR;
    copy_command_inputs(command); assert(waits == ++before);
    // Unknown command types keep the conservative completion guarantee.
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    command->op = CMD_COUNT;
    copy_command_inputs(command); assert(waits == ++before);
    free(command);
    puts("PASS: target changes/unrelated clears stay queued; attachment reads, counter writes/paired reads, source lifetime and unknown commands wait");
}
''')
binary = out / 'test-buffer-copy-commands'
subprocess.run([os.environ.get('CC', 'clang'), '-D__ANDROID__', '-O1', '-g', '-fsanitize=address,undefined',
    '-ffunction-sections', '-fdata-sections', '-I' + str(root / 'tools'),
    '-I' + str(root / 'android/deps/out/include'), '-I' + str(root / 'android/deps/out/volk'),
    str(unit), '-Wl,--gc-sections', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=10)
