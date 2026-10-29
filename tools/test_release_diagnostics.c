// Exercise production stubs with hostile diagnostic environment variables, and
// keep the scheduler's function-entry preemption hook working in Release.
#include "../runtime/test_session.h"
#include "../runtime/test_boot.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0);
void fnprof_frame(uint64_t frame);
static unsigned preemptions;
void kernel_preempt_point(void) { ++preemptions; }

int main(void) {
    setenv("LARECOMP_FNPROF", "0:100", 1);
    setenv("LARECOMP_TEST_SESSION", "/must-not-be-created", 1);
    setenv("LARECOMP_FULL_GPU_CAPTURE", "1", 1);
    assert(!diag_getenv("LARECOMP_FULL_GPU_CAPTURE"));
    assert(!test_session_enabled() && !test_session_timed());
    assert(test_session_save_slot() == -1 && test_session_target_fps() == 0);
    assert(!test_session_buttons(120) && !test_session_camera_x());
    assert(!test_session_take_gpu_request() && !test_session_take_shot_request());
    assert(!test_boot_target("Sequence/OnLoadGameSaveData.module.ainb"));
    test_session_frame(100, true, NULL);
    test_session_poll();
    fnprof_frame(0);
    assert(g_fnprof == 0);
    fnprof_hit(1, 2, 3);
    assert(preemptions == 0);
    g_fnprof = 2;
    fnprof_hit(1, 2, 3);
    assert(preemptions == 1 && g_fnprof == 2);
    g_fnprof = 0;
    fnprof_hit(1, 2, 3);
    assert(preemptions == 1);
    puts("PASS: Release rejects diagnostic activation and preserves scheduler preemption");
}
