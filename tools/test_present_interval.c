// Exercise the production NVN setters/getters/present path, not a copy of their selection algorithm.
#ifndef TOTK_DIAGNOSTICS
#define TOTK_DIAGNOSTICS 1
#endif
#include <assert.h>
#include "../runtime/nvn/objects.c"
#include "../runtime/hle/framepace.c"

int g_game_version = 143;
uint64_t g_guest_lo;
Settings g_settings = {.fps_mode = 2};
volatile uint64_t g_vsync_count;
__thread uint64_t g_blocked_ns;
#if TOTK_DIAGNOSTICS
int test_session_target_fps(void) { return 0; }
uint64_t renderer_present_count(void) { return 0; }
void kernel_wait_probe_frame(uint64_t frame) { (void)frame; }
#endif
uint64_t plat_now_ns(void) { return 1000000000ull; }
void plat_sleep_until_ns(uint64_t ns) { (void)ns; assert(!"test present waiting disabled"); }
void backend_present(NvnTexture *t) { (void)t; assert(!"test does not bind a texture"); }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; abort(); }
#if TOTK_DIAGNOSTICS
bool vk_timing_trace_on(void) { return false; }
void vk_timing_trace_wait(int kind, uint64_t ns) { (void)kind; (void)ns; }
#endif

static void check(NvnWindow *w, uint64_t *object, int request, int expected) {
    Cpu c = {0};
    c.x[0] = (uintptr_t)object; c.x[1] = (uint64_t)request;
    hle_nvnWindowSetPresentInterval(&c);
    assert(w->interval == expected && g_present_interval == expected);
    c.x[0] = (uintptr_t)object;
    hle_nvnWindowGetPresentInterval(&c); assert(c.x[0] == (uint64_t)expected);
    // The game does not necessarily call SetInterval after a menu/movie changes the target.
    w->interval = request;
    c.x[0] = (uintptr_t)object;
    hle_nvnWindowGetPresentInterval(&c); assert(c.x[0] == (uint64_t)expected);
    c.x[1] = (uintptr_t)object; c.x[2] = UINT64_MAX;
    hle_nvnQueuePresentTexture(&c);
    assert(w->interval == expected && g_present_interval == expected);
}

int main(int argc, char **argv) {
    bool off = argc > 1 && !strcmp(argv[1], "off");
    bool force = argc > 1 && !strcmp(argv[1], "force");
    setenv("LARECOMP_PRESENT_WAIT", "0", 1);
    if (off) setenv("LARECOMP_FRAMEPACE", "0", 1);
    if (force) setenv("LARECOMP_FORCE_INTERVAL", "3", 1);
    else unsetenv("LARECOMP_FORCE_INTERVAL");
    setenv("LARECOMP_TOTK_CUTSCENE_MARKS", "1", 1);
    NvnWindow w = {0}; uint64_t object = (uintptr_t)&w;
    const int modes[] = {2, 1, 3, 4, 0};
    const int intervals[] = {1, 2, 1, 1, 1};
    for (unsigned m = 0; m < 5; ++m) {
        g_settings.fps_mode = modes[m];
        for (int resolution = 1; resolution <= 2; ++resolution) {
            g_settings.res_mode = resolution;
            for (int request = 0; request < 5; ++request)
                check(&w, &object, request, force && TOTK_DIAGNOSTICS ? 3 : off ? request : intervals[m]);
        }
    }
    if (!off && !(force && TOTK_DIAGNOSTICS)) {
        g_settings.fps_mode = 2; g_movie_playing = 1;
        check(&w, &object, 1, 2);
        g_movie_playing = 0; check(&w, &object, 2, 1);
        Cpu mark = {0}; rt_totk_frame_hook(&mark, 9);
        check(&w, &object, 1, TOTK_DIAGNOSTICS ? 2 : 1);
        framepace_totk_reset();
    }
    // Unsupported games retain the original NVN state (apart from the explicit diagnostic override).
    g_game_version = 999;
    for (int request = 0; request < 5; ++request)
        check(&w, &object, request, force && TOTK_DIAGNOSTICS ? 3 : request);
    puts("PASS: NVN intervals follow the target, stale state/movie transitions, disabled pacing and diagnostic isolation");
}
