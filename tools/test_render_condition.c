// Record the real NVN command and replay TotK's one-time-clear protocol.
// Counter values change after recording; timestamps must not affect the result.
#include <assert.h>
#include "../runtime/nvn/cmdbuf.c"
#include "../runtime/nvn/render_control.h"

void rt_fatal_trace(void) { abort(); }
void guest_call(Cpu *c, uint64_t address) { (void)c; (void)address; assert(!"unexpected guest callback"); }

int main(void) {
    uint64_t object = 0, counters[4] = {0, 123, 0, 456};
    Cpu cpu = {0};
    cpu.x[0] = (uintptr_t)&object;
    hle_nvnCommandBufferInitialize(&cpu);
    cpu.x[0] = (uintptr_t)&object;
    hle_nvnCommandBufferBeginRecording(&cpu);
    cpu.x[0] = (uintptr_t)&object;
    cpu.x[1] = 1;  // render if the counter fields differ
    cpu.x[2] = (uintptr_t)counters;
    hle_nvnCommandBufferSetRenderEnableConditional(&cpu);
    cpu.x[0] = (uintptr_t)&object;
    hle_nvnCommandBufferEndRecording(&cpu);
    CmdRec *head = (void *)(uintptr_t)cpu.x[0];
    CmdRec *condition = (void *)((uint8_t *)head + head->size);
    assert(condition->op == CMD_SET_RENDER_ENABLE_CONDITIONAL);
    assert(condition->x[0] == 1 && condition->x[1] == (uintptr_t)counters);

    uint32_t image[80 * 45];
    for (unsigned i = 0; i < 80 * 45; i++) image[i] = 0x12345678;
    unsigned clears = 0;
    for (unsigned replay = 0; replay < 3; replay++) {
        if (!replay) counters[0] = 1;  // CPU arms the already recorded list
        uint64_t a, b;
        if (nvn_render_condition((void *)(uintptr_t)condition->x[1], condition->x[0], &a, &b)) {
            for (unsigned y = 0; y < 22; y++) for (unsigned x = 0; x < 40; x++) image[y * 80 + x] = 0;
            clears++;
        }
        counters[0] = 0;  // TIMESTAMP_TOP reports zero in the counter lane
        if (!replay) for (unsigned i = 0; i < 80 * 45; i++) image[i] = 0xaabbccdd;
    }
    assert(clears == 1);
    for (unsigned i = 0; i < 80 * 45; i++) assert(image[i] == 0xaabbccdd);
    uint64_t a, b;
    assert(nvn_render_condition(counters, 0, &a, &b)); // unequal timestamps, equal counters
    counters[0] = 1ull << 40;
    assert(!nvn_render_condition(counters, 0, &a, &b)); // compare the full 64 bits
    assert(nvn_render_condition(counters, 1, &a, &b));
    counters[2] = counters[0];
    assert(nvn_render_condition(counters, 0, &a, &b));
    free((void *)(uintptr_t)HOST(NvnCmdBuf, (uintptr_t)&object)->mem);
    cpu.x[0] = (uintptr_t)&object;
    hle_nvnCommandBufferFinalize(&cpu);
    puts("conditional render recording and one-time clear replay passed");
}
