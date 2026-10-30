// CPU-only timing and hook regression. Generated instructions are checked separately by test_totk_framepace.py.
#define __ANDROID__ 1
#define TOTK_DIAGNOSTICS 1 // fixture exercises the explicit test-session FPS override
#include <assert.h>
#include "../runtime/hle/framepace.c"

int g_game_version = 143;
uint64_t g_guest_lo;
Settings g_settings = {.fps_mode = 1};
static uint64_t test_now = 1000000000ull;
uint64_t plat_now_ns(void) { return test_now; }
static int test_fps;
int test_session_target_fps(void) { return test_fps; }

static void near(double actual, double expected, double tolerance) { assert(fabs(actual - expected) < tolerance); }
static void rate(int fps, int mode) {
    g_settings.fps_mode = mode;
    assert(framepace_target() == fps);
    for (int requested = 0; requested <= 4; ++requested)
        assert(framepace_present_interval(requested) == (fps == 30 ? 2 : 1));
    for (int requested = 1; requested <= 4; ++requested) {
        Cpu pick = {0}, before;
        pick.x[21] = requested; pick.x[8] = 0x12345678; pick.nzcv = 0xa0000000;
        before = pick; before.x[21] = fps == 30 ? 2 : 1;
        rt_totk_frame_hook(&pick, 6);
        assert(!memcmp(&pick, &before, sizeof pick));
    }
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
    bool marks = argc > 1 && !strcmp(argv[1], "marks");
    if (marks) setenv("LARECOMP_TOTK_CUTSCENE_MARKS", "1", 1);
    if (argc > 1 && !marks) {
        // In a fresh process, the diagnostic off switch preserves every register and guest stack byte.
        setenv("LARECOMP_FRAMEPACE", "0", 1);
        Cpu c, before; memset(&c, 0xab, sizeof c); before = c;
        for (int i = 0; i < 13; i++) rt_totk_frame_hook(&c, i);
        assert(!framepace_on() && !memcmp(&c, &before, sizeof c));
        assert(framepace_present_interval(3) == 3);
        puts("TotK frame pacing off: original semantics preserved"); return 0;
    }
    assert(framepace_on());
    rate(30, 1); rate(45, 3); rate(60, 2); rate(120, 4);
    g_settings.fps_mode=1; test_fps=60;
    assert(framepace_present_interval(3)==1); // The benchmark override takes precedence over a 30-FPS seed.
    test_fps=45; assert(framepace_target()==45); test_fps=0;
    g_settings.fps_mode=4; assert(framepace_target()==120);
    // Movie cap changes the target and resets sampling; returning to gameplay restores it.
    g_settings.fps_mode = 4; g_movie_playing = 1; assert(framepace_target() == 30);
    assert(framepace_present_interval(1)==2);
    g_movie_playing = 0; assert(framepace_target() == 120);
    assert(framepace_present_interval(2)==1);

    // Each cutscene chain independently caps the same target/interval; no register, flag or stack changes.
    for (int site = 7; marks && site <= 10; ++site) {
        framepace_totk_reset();
        Cpu mark = {0}, before; mark.x[0] = 0xabcdef; mark.nzcv = 0xa0000000; before = mark;
        rt_totk_frame_hook(&mark, site);
        assert(!memcmp(&mark, &before, sizeof mark));
        assert(framepace_target() == 30 && framepace_present_interval(1) == 2);
        Cpu pick = {0}; pick.x[21] = 3; rt_totk_frame_hook(&pick, 6); assert(pick.x[21] == 2);
        test_now += 199999999;
        assert(framepace_target() == 30);
        test_now++;
        assert(framepace_target() == 120 && framepace_present_interval(3) == 1);
    }
    Cpu mark = {0}; rt_totk_frame_hook(&mark, 7);
    if (marks) {
        test_now--; assert(framepace_target() == 120); // a backwards host clock does not extend the mark
        test_now++; assert(framepace_target() == 30);
    } else {
        assert(framepace_target() == 120 && atomic_load(&totk_cutscene_ns) == 0);
        for (int site = 7; site <= 10; ++site) rt_totk_frame_hook(&mark, site);
        assert(framepace_target() == 120 && framepace_present_interval(3) == 1);
    }
    framepace_totk_reset(); assert(framepace_target() == 120); // no stale state on in-place reboot

    // Only zero results of a positive fade are advanced; native conversion results and all other state survive.
    for (int site = 11; site <= 12; ++site) {
        unsigned rd = site == 11 ? 8 : 10;
        const float values[] = {0, -0.0f, -0.5f, 0.25f, 0.99f, 1.5f, NAN};
        const uint32_t results[] = {0, 0, 0, 0, 0, 1, 0};
        const uint32_t expected[] = {0, 0, 0, 1, 1, 1, 0};
        for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
            Cpu fade = {0}, before; fade.nzcv = 0xa0000000;
            frame_set_s(&fade, 0, values[i]); fade.x[rd] = results[i];
            before = fade; before.x[rd] = expected[i];
            rt_totk_frame_hook(&fade, site); assert(!memcmp(&fade, &before, sizeof fade));
        }
    }

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
    assert(framepace_present_interval(3)==3);
    Cpu c = {0}; frame_set_s(&c, 0, 7); rt_totk_frame_hook(&c, 0); assert(frame_s(&c, 0) == 7);
    puts("TotK 30/45/60/120 passed: real clock, controller interval, cutscene expiry/reset, fades, cloth, bounce, jitter");
}
