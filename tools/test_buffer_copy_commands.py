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
texture_ref = next(line for line in execution.splitlines() if line.startswith('typedef struct') and line.endswith('} TexRefRec;'))
unit = out / 'buffer-copy-commands.c'
unit.write_text(fixture + r'''
static struct { struct { int ncolor; NvnTexture *color[8], *depth; } rt; } st;
uint64_t tegra_total_size(const TexLayout *layout) { return layout->width; }
''' + texture_ref + helper('copy_texture_input') + helper('copy_command_inputs') + r'''
int main(void) {
    setenv("LARECOMP_VK_ASYNC_BUFFER_COPY", "1", 1);
    memset(memory, 0x71, 32);
    CmdRec *command = calloc(1, sizeof(CmdRec) + 2 * sizeof(TexRefRec));
    assert(command);
    unsigned before = waits;
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    command->op = CMD_SET_RENDER_TARGETS;
    copy_command_inputs(command);
    assert(waits == before && vk_buffer_copy_pending() && !memory[0x100]);
    const unsigned setters[] = {CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_LINE_WIDTH,
                               CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK};
    for (unsigned i = 0; i < sizeof setters / sizeof *setters; ++i) {
        command->op = setters[i]; copy_command_inputs(command);
        assert(waits == before && vk_buffer_copy_pending());
    }
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
    // A completed copy must not disable preflight for a later direct GPU read.
    assert(vk_buffer_copy_async(0x10000, 0x10110, 16));
    vk_buffer_copy_uniform_begin();
    unsigned ticket = vk_buffer_copy_uniform_reserve(0x10100, 32);
    assert(ticket);
    complete(); vk_buffer_copy_cpu_read(0x10110, 16);
    assert(!vk_buffer_copy_pending());
    vk_buffer_copy_uniform_use(ticket);
    command->op = CMD_UPDATE_UNIFORM_BUFFER;
    command->x[0] = 0x10100; command->x[2] = 0; command->x[3] = 4;
    copy_command_inputs(command); assert(waits == ++before);
    const unsigned image_copies[] = {CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE};
    TexRefRec *refs = (TexRefRec *)command->extra;
    refs[0].tex = (uintptr_t)&color; refs[1].tex = (uintptr_t)&depth;
    for (unsigned i = 0; i < 2; ++i) {
        color.addr = 0x11000; depth.addr = 0x11200;
        command->op = image_copies[i];
        assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
        copy_command_inputs(command); assert(waits == before && vk_buffer_copy_pending());
        color.addr = 0x1011c; // source texels may need uploading
        copy_command_inputs(command); assert(waits == ++before);
        assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
        color.addr = 0x11000; depth.addr = 0x1011c; // destination's previous texels too
        copy_command_inputs(command); assert(waits == ++before);
    }
    // Event storage is a single word, not a write to every guest allocation.
    uint64_t event = 0x11000;
    command->op = CMD_SIGNAL_EVENT; command->x[0] = (uintptr_t)&event;
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    copy_command_inputs(command); assert(waits == before && vk_buffer_copy_pending());
    event = 0x1011c; copy_command_inputs(command); assert(waits == ++before);
    assert(vk_buffer_copy_async(0x10000, 0x10100, 32));
    event = 0x10000; copy_command_inputs(command); assert(waits == ++before);
    free(command);
    puts("PASS: scalar state/image copies stay queued; overlapping texels, counters, copy/uniform lifetimes and unknown commands wait");
}
''')
binary = out / 'test-buffer-copy-commands'
subprocess.run([os.environ.get('CC', 'clang'), '-D__ANDROID__', '-O1', '-g', '-fsanitize=address,undefined',
    '-ffunction-sections', '-fdata-sections', '-I' + str(root / 'tools'),
    '-I' + str(root / 'android/deps/out/include'), '-I' + str(root / 'android/deps/out/volk'),
    str(unit), '-Wl,--gc-sections', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=10)
