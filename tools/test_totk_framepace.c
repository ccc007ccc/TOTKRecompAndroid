// CPU-only timing and hook regression. Generated instructions are checked separately by test_totk_framepace.py.
#define __ANDROID__ 1
#include <assert.h>
#include "../runtime/hle/framepace.c"

int g_game_version = 143;
uint64_t g_guest_lo;
Settings g_settings = {.fps_mode = 1};
uint64_t plat_now_ns(void) { return 1; }

static void near(double actual, double expected, double tolerance) { assert(fabs(actual - expected) < tolerance); }
static void rate(int fps, int mode) {
    g_settings.fps_mode = mode;
    assert(framepace_target() == fps);
    uint64_t period = framepace_period_ns(), t = 1000000000ull;
    near(period * 1e-9, 1.0 / fps, 1e-9);
    framepace_totk_reset();
    framepace_presented(t, true);
    for (int i = 0; i < 60; i++) framepace_presented(t += period, true);
    double seconds = atomic_load(&totk_seconds);
    near(seconds, 1.0 / fps, 1e-8);

    Cpu c = {0};
    c.nzcv = 0xa0000000; c.x[9] = 0x11223344; c.x[30] = 0x5678;
    frame_set_s(&c, 0, 2); frame_set_s(&c, 1, 0.5);
    rt_totk_frame_hook(&c, 0);
    near(frame_s(&c, 0) * frame_s(&c, 1), seconds * 30, 1e-6); // producer's half selector retained
    frame_set_s(&c, 1, 1); rt_totk_frame_hook(&c, 0);
    near(frame_s(&c, 0) * frame_s(&c, 1), seconds * 60, 1e-6);
    assert(c.nzcv == 0xa0000000 && c.x[9] == 0x11223344 && c.x[30] == 0x5678);
    frame_set_s(&c, 1, 0.5); rt_totk_frame_hook(&c, 1);
    near(frame_s(&c, 0) * frame_s(&c, 1), 30.0 / fps, 1e-6);

    unsigned char stack[1024] = {0}; c.sp = (uintptr_t)stack;
    float factors[] = {0, 0.25f, 1, 2};
    for (unsigned i = 0; i < 4; i++) {
        float factor = factors[i], ratio;
        frame_set_s(&c, 10, factor); rt_totk_frame_hook(&c, 2);
        memcpy(&ratio, stack + 0x364, 4);
        near(ratio, factor * seconds * 30, 1e-6);
        near(frame_s(&c, 0) * frame_s(&c, 10), factor * seconds, 1e-7);
        frame_set_s(&c, 0, factor); rt_totk_frame_hook(&c, 3);
        memcpy(&ratio, stack + 0x114, 4);
        near(ratio, factor * seconds * 30, 1e-6);
        near(frame_s(&c, 0) * frame_s(&c, 1), factor * seconds, 1e-7);
        frame_set_s(&c, 0, 7); frame_set_s(&c, 1, 8);
        frame_set_s(&c, 2, factor / 30); frame_set_s(&c, 3, factor);
        rt_totk_frame_hook(&c, 4);
        near(frame_s(&c, 2), factor * seconds, 1e-7);
        near(frame_s(&c, 3), factor * seconds * 30, 1e-6);
        assert(frame_s(&c, 0) == 7 && frame_s(&c, 1) == 8 && c.x[30] == 0x5678);
    }
    // Bounce threshold still distinguishes slow motion, regardless of selected FPS.
    float time_object[16] = {0}; time_object[0x38 / 4] = seconds * 30;
    c.x[9] = (uintptr_t)time_object;
    frame_set_s(&c, 0, 1 / seconds); rt_totk_frame_hook(&c, 5);
    near(frame_s(&c, 0), 30, 1e-4); assert(frame_s(&c, 0) <= 100);
    frame_set_s(&c, 0, 1 / (seconds * 0.2)); rt_totk_frame_hook(&c, 5);
    near(frame_s(&c, 0), 150, 1e-4); assert(frame_s(&c, 0) > 100);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        // In a fresh process, the diagnostic off switch preserves every register and guest stack byte.
        setenv("LARECOMP_FRAMEPACE", "0", 1);
        Cpu c, before; memset(&c, 0xab, sizeof c); before = c;
        for (int i = 0; i < 6; i++) rt_totk_frame_hook(&c, i);
        assert(!framepace_on() && !memcmp(&c, &before, sizeof c));
        puts("TotK frame pacing off: original semantics preserved"); return 0;
    }
    assert(framepace_on());
    rate(30, 1); rate(45, 3); rate(60, 2); rate(120, 4);
    // Movie cap changes the target and resets sampling; returning to gameplay restores it.
    g_settings.fps_mode = 4; g_movie_playing = 1; assert(framepace_target() == 30);
    g_movie_playing = 0; assert(framepace_target() == 120);

    FrameClock clock = {0};
    uint64_t t = 1000000000, period = 1000000000 / 120;
    frame_clock_update(&clock, t, period);
    double given = 0, real = 0;
    // Alternating 7 / 9.667 ms frames average 120; a 1/120 lower clamp would run fast.
    for (int i = 0; i < 72000; i++) {
        uint64_t dt = i & 1 ? 9666666 : 7000000;
        given += frame_clock_update(&clock, t += dt, period); real += dt * 1e-9;
    }
    near(given, real, 0.005);
    near(frame_clock_update(&clock, t += 30000000000ull, period), period * 1e-9, 1e-12);
    near(frame_clock_update(&clock, t, period), period * 1e-9, 1e-12); // repeated timestamp
    near(frame_clock_update(&clock, t - 1, period), period * 1e-9, 1e-12); // reset/backwards clock
    for (int i = 0; i < 10; i++) given = frame_clock_update(&clock, t += 100000000, period);
    near(given, 1.0 / 15, 1e-12); // bounded physics step below 15 FPS
    near(frame_clock_update(&clock, t += 22222222, 22222222), 22222222e-9, 1e-12);
    for (int i = 0; i < 10; i++) given = frame_clock_update(&clock, t += 40000000, 22222222);
    near(given, 0.04, 1e-12); // 45 target, 25 actual: simulation follows achievable rate
    g_game_version = 999; assert(!framepace_on());
    Cpu c = {0}; frame_set_s(&c, 0, 7); rt_totk_frame_hook(&c, 0); assert(frame_s(&c, 0) == 7);
    puts("TotK 30/45/60/120 passed: real clock, selector, controller, cloth, bounce, jitter, pause, low FPS, switching");
}
