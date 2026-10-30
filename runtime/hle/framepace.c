// TotK 1.4.3: 30/45/60/120 FPS, measured time at the producer and secondary animation clock (TotK section below).
// BOTW 1.8.2: 30, 60 or no limit (Video > Frame rate), and the game's clock the same at every one of
// them - from SavageWindRecomp (MIT, runtime/hle/framepace.c), which worked this out for its PC port of 1.8.2.
//
// How BOTW 1.8.2 keeps time (main module, canonical addresses):
//   - Its frame loop (sead GameFrameworkNx, 0x700101287c) asks for a present interval of 2 vsyncs (30 fps), 3 (20 fps)
//     after three slow frames in a row. The frame loop's own vsync wait (0x7000d811fc) only runs for the first frames
//     after start-up; the present keeps the pace.
//   - ksys::VFR (the instance at [0x7001d32010]) turns that interval into the game's time step: one "delta frame" is
//     1/30 s. Every frame VFR::updateInterval (0x7001409230) sets the interval ratio (interval / 2: 1.0 at 30 fps,
//     1.5 at 20 fps), and delta frame = ratio x the slowest active time multiplier (slow motion). About 1600 call
//     sites read it: the time of day, the Blood Moon timer, the Havok step, stamina, animation, AI.
//   - The paused game (pause menu, map: the state at 0x7001cb48a4 is 2, 0 while playing) updates VFR twice a frame.
// So the game already runs at any frame rate as long as VFR is told how long a frame really took. The runtime:
//   - paces the presents at the chosen rate on a steady grid (nvnQueuePresentTexture, framepace_period_ns), and skips
//     the frame loop's vsync wait (rt_frame_wait, tools/patches.py);
//   - tells VFR how long the frame really took (rt_vfr_update, inside updateInterval after the game set its ratio):
//     ratio = seconds x 30, changing by a few percent a frame at most, and whatever it gave more or less than the
//     real time is paid back over the next frames. The interval follows the frame rate averaged over ~20 frames (1
//     above ~43 fps, 2 below ~35, 3 below ~23). A step is 1/20 s at most: frames slower than that for long slow the
//     game down, as on the console, instead of taking steps big enough to break the physics;
//   - holds 30 fps in the menus (title screen, pause menu, map), the events (cutscenes, talks: some of their waits
//     count frames) and while a movie plays (movie.c);
//   - steps the controllers' hold counters by the frame's length (CONTROLLER_STEP).
// Switches:
//   LARECOMP_FPS=<n>             frame rate instead of the setting (0: no limit)
//   LARECOMP_FRAMEPACE=0         the game's own pacing and clock (its 30 / 20 fps, as before)
//   LARECOMP_MOVIE_FPS / LARECOMP_MENU_FPS / LARECOMP_EVENT_FPS=<n>  the frame rate there (default 30; 0: the chosen one)
//   LARECOMP_LOG_FRAMEPACE=<n>   every n clock updates: target, frame length, VFR ratio and interval
//   LARECOMP_NO_CONTROLLER_STEP=1  the controllers count presented frames (as the game does)
#include "../diagnostics_build.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"
#include "../framepace.h"
#include "../frame_clock.h"
#include "../test_session.h"

volatile int g_movie_playing;  // FFmpeg decoder count; zero in builds without FFmpeg

// a canonical address of 1.8.2's main (0x7000000000 + offset) where the guest window is
#define GA(a) ((uint64_t)(a) - 0x7000000000ull + GUEST_LO)
#define VF(T, base, off) (*(T *)(uintptr_t)((base) + (off)))
#define PAUSE_STATE GA(0x7001cb48a4ull)  // (u32: 0 playing, 2 in the pause menu or the map)

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

bool framepace_on(void) {
    static int on = -1;
    if (on < 0) on = env_int("LARECOMP_FRAMEPACE", 1) != 0;
    return on && (g_game_version == 182 || g_game_version == 143);
}

// ------------------------------------------------------------------ the frame rate
// The menus: the game is paused (pause menu, map), or the title screen's menu is up (screen 1 in the UI screen
// manager [0x7001c05d88]: +0x28 count, +0x30 screens by id).
static bool in_menu(void) {
    if (*(uint32_t *)(uintptr_t)PAUSE_STATE) return true;
    uint64_t mgr = *(uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)GA(0x7001c05d88ull);
    return mgr && VF(uint32_t, mgr, 0x28) > 1 && VF(uint64_t, mgr, 0x30) &&
           *(uint64_t *)(uintptr_t)(VF(uint64_t, mgr, 0x30) + 8);
}

// (widescreen.c: the HUD stays in place in the menus)
bool framepace_in_menu(void) { return g_game_version == 182 && in_menu(); }

// (widescreen.c) the title screen's menu is up - screen 1 in the UI screen manager, as in_menu - and the game isn't paused
bool framepace_on_title(void) {
    if (g_game_version != 182) return false;
    uint64_t holder = *(uint64_t *)(uintptr_t)GA(0x7001c05d88ull);
    uint64_t mgr = holder ? *(uint64_t *)(uintptr_t)holder : 0;
    bool on = mgr && !*(uint32_t *)(uintptr_t)PAUSE_STATE && VF(uint32_t, mgr, 0x28) > 1 && VF(uint64_t, mgr, 0x30) &&
              *(uint64_t *)(uintptr_t)(VF(uint64_t, mgr, 0x30) + 8);
    static int last = -1;
    if (diag_getenv("LARECOMP_LOG_TITLE") && on != last) {
        last = on;
        LOG("[framepace] title screen %d (screens %u, menu %d)\n", on, mgr ? VF(uint32_t, mgr, 0x28) : 0, in_menu());
    }
    return on;
}

// An event is running: a cutscene, a talk, an item fanfare (ksys::evt::Manager [0x7001cd8db8]: its active event at
// +0x1d2b8). Some of their waits count frames: the new game's opening gave Link control 2 s early at 60 fps.
static bool in_event(void) {
    uint64_t evt = *(uint64_t *)(uintptr_t)GA(0x7001cd8db8ull);
    return evt && *(uint64_t *)(uintptr_t)(evt + 0x1d2b8);
}

// mamcjr/TOTKRecompAndroid da06d5a7f3fb1378eeefbcb13dc2276096a1aebc (MIT): four
// candidate cutscene call chains. main+0x42c3bc also runs in our grassland gameplay, so these marks are
// diagnostic-only until a reliable game-state detector replaces them. Do not cap normal gameplay with them.
static _Atomic uint64_t totk_cutscene_ns;
static bool totk_cutscene_marks_on(void) {
    static _Atomic int on = -1;
    int value = atomic_load_explicit(&on, memory_order_relaxed);
    if (value < 0) {
        const char *e = diag_getenv("LARECOMP_TOTK_CUTSCENE_MARKS");
        value = e && atoi(e) != 0;
        atomic_store_explicit(&on, value, memory_order_relaxed);
    }
    return value != 0;
}
static bool totk_in_cutscene(void) {
    uint64_t t = atomic_load_explicit(&totk_cutscene_ns, memory_order_relaxed);
    if (!t) return false;
    uint64_t now = plat_now_ns();
    return now >= t && now - t < 200000000ull;
}

int framepace_target(void) {
    static int forced = -2, movie = -2, menu = -2, event = -2;
    if (forced == -2) forced = env_int("LARECOMP_FPS", -1);
    if (movie == -2) movie = env_int("LARECOMP_MOVIE_FPS", 30);
    if (menu == -2) menu = env_int("LARECOMP_MENU_FPS", 30);
    if (event == -2) event = env_int("LARECOMP_EVENT_FPS", 30);
    // (Video > Frame rate: 30, 60; Auto is 60 here - the clock follows whatever the frames really take)
    int test_fps = test_session_target_fps();
    int fps = test_fps > 0 ? test_fps : forced >= 0 ? forced : g_settings.fps_mode == 1 ? 30 :
              g_settings.fps_mode == 3 ? 45 : g_settings.fps_mode == 4 ? 120 : 60;
    if (g_game_version == 143 && (fps == 0 || fps > 120)) fps = 120;
    if (movie > 0 && __atomic_load_n(&g_movie_playing, __ATOMIC_RELAXED) > 0 && (fps == 0 || fps > movie)) fps = movie;
    if (g_game_version == 182 && menu > 0 && (fps == 0 || fps > menu) && in_menu()) fps = menu;
    if (g_game_version == 182 && event > 0 && (fps == 0 || fps > event) && in_event()) fps = event;
    if (g_game_version == 143 && event > 0 && fps > event && totk_in_cutscene()) fps = event;
    return fps;
}

// No limit still stops at 300 fps: shorter steps lose precision in the game's float clocks (the time of day, 0..360,
// takes 1/30 of a minute times the step each frame).
#define FPS_CEILING 300

uint64_t framepace_period_ns(void) {
    int fps = framepace_target();
    return 1000000000ull / (uint64_t)(fps > 0 ? fps : FPS_CEILING);
}

int framepace_present_interval(int requested) {
    if (g_game_version != 143 || !framepace_on()) return requested;
    int fps = framepace_target();
    if (fps <= 0) return requested;
    int interval = 60 / fps;
    return interval > 0 ? interval : 1;
}

// The frame loop's vsync wait: the present keeps the pace instead (1: skipped; 0: the game's own wait).
int rt_frame_wait(void) { return framepace_on(); }

// ------------------------------------------------------------------ presents
// The controllers (sead::ControllerBase::updateDerivativeParams_, 0x7000d9d8a0) count how long each button has been
// held, in frames, by a step the game keeps at 1 (a float at 0x7001c4ba40, read there only): the runtime sets it to
// the frame's length in console frames, so holds last as long as on the console.
#define CONTROLLER_STEP GA(0x7001c4ba40ull)
static float g_step = 1;   // the last frame's length between presents, in console frames
static int g_grid_streak;  // presents in a row that kept the pace

// TotK 1.4.3: the generator hooks the time producer, preserving the original half-step selector, FrameRateMgr
// arithmetic and intentional time multipliers. Cloth/hair has a second clock, corrected only at four actor sites.
// Addresses/mechanism: MacNativeGames game_patches_v143.c @ff63a162 (MIT; LICENSE-MacNativeGames.txt).
static FrameClock totk_clock;
static _Atomic float totk_seconds = 1.0f / 30.0f;
static _Atomic unsigned totk_hook_seen;
static _Atomic int totk_requested_interval, totk_effective_interval;
RT_ON_RESET(framepace_totk_reset) {
    totk_clock = (FrameClock){0};
    atomic_store_explicit(&totk_seconds, 1.0f / 30.0f, memory_order_relaxed);
    atomic_store_explicit(&totk_hook_seen, 0, memory_order_relaxed);
    atomic_store_explicit(&totk_cutscene_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&totk_requested_interval, 0, memory_order_relaxed);
    atomic_store_explicit(&totk_effective_interval, 0, memory_order_relaxed);
}

static float frame_s(const Cpu *c, unsigned reg) {
    float f; memcpy(&f, &c->v[reg], sizeof f); return f;
}
static void frame_set_s(Cpu *c, unsigned reg, float value) {
    c->v[reg] = 0; memcpy(&c->v[reg], &value, sizeof value);
}

void rt_totk_frame_hook(Cpu *c, int site) {
    if (g_game_version != 143 || !framepace_on()) return;
    if (site < 0 || site > 12) return;
    if (site >= 7 && site <= 10 && !totk_cutscene_marks_on()) return;
    unsigned bit = 1u << site;
    if (!(atomic_load_explicit(&totk_hook_seen, memory_order_relaxed) & bit) &&
        !(atomic_fetch_or_explicit(&totk_hook_seen, bit, memory_order_relaxed) & bit))
        LOG("[framepace] TotK timing hook %d active\n", site);
    float seconds = atomic_load_explicit(&totk_seconds, memory_order_relaxed);
    float ratio = seconds * 30.0f;
    switch (site) {
    case 0: // original fmul s8,s0,s1 retains sys+0x1da's half/full selector
    case 1: // original fmul s0,s0,s1 (s1=0.5) produces the controller's 30 Hz step
        frame_set_s(c, 0, seconds * 60.0f);
        break;
    case 2: { // secondary animation, first inline actor
        float step = frame_s(c, 10) * ratio;
        memcpy((void *)(uintptr_t)(c->sp + 0x364), &step, sizeof step);
        frame_set_s(c, 0, seconds);
        break;
    }
    case 3: { // second inline actor; the ordinary animation arguments remain untouched
        float step = frame_s(c, 0) * ratio;
        memcpy((void *)(uintptr_t)(c->sp + 0x114), &step, sizeof step);
        frame_set_s(c, 1, seconds);
        break;
    }
    case 4: // the two nominal-time callers of 0x2a0e40; preserve the measured-time caller
        frame_set_s(c, 2, frame_s(c, 2) * ratio);
        frame_set_s(c, 3, frame_s(c, 3) * ratio);
        break;
    case 5: { // X+0x28 is FPS including slow motion. Multiply by X+0x38 to remove the frame-rate component.
        float step;
        memcpy(&step, (const void *)(uintptr_t)(c->x[9] + 0x38), sizeof step);
        frame_set_s(c, 0, frame_s(c, 0) * step);
        break;
    }
    case 6: { // main+0x2db238: every interval choice meets here BEFORE the game's state stores and NVN call.
        int requested = (int)(uint32_t)c->x[21];
        int effective = framepace_present_interval(requested);
        c->x[21] = (uint32_t)effective;
        atomic_store_explicit(&totk_requested_interval, requested, memory_order_relaxed);
        atomic_store_explicit(&totk_effective_interval, effective, memory_order_relaxed);
        break;
    }
    case 7: case 8: case 9: case 10: // original cutscene entry instruction still executes after this mark
        atomic_store_explicit(&totk_cutscene_ns, plat_now_ns(), memory_order_relaxed);
        break;
    case 11: case 12: { // AFTER the original FCVTZS: a positive fade step cannot truncate to zero.
        unsigned rd = site == 11 ? 8 : 10;
        if ((uint32_t)c->x[rd] == 0 && frame_s(c, 0) > 0) c->x[rd] = 1;
        break;
    }
    }
}

void framepace_presented(uint64_t t, bool on_grid) {
    if (!framepace_on()) return;
    if (g_game_version == 143) {
        float seconds = (float)frame_clock_update(&totk_clock, t, framepace_period_ns());
        atomic_store_explicit(&totk_seconds, seconds, memory_order_relaxed);
        static int every = -1;
        static uint64_t frames;
        if (every < 0) every = env_int("LARECOMP_LOG_FRAMEPACE", 0);
        if (every > 0 && frames++ % (uint64_t)every == 0)
            LOG("[framepace] TotK target %d fps, measured step %.3f ms, ratio %.4f, interval %d -> %d%s\n",
                framepace_target(), seconds * 1000.0, seconds * 30.0,
                atomic_load_explicit(&totk_requested_interval, memory_order_relaxed),
                atomic_load_explicit(&totk_effective_interval, memory_order_relaxed), on_grid ? ", on grid" : "");
        return;
    }
    static uint64_t prev;
    static double smooth;
    double s = prev && t > prev ? (t - prev) / 1e9 : 1.0 / 30;
    prev = t;
    if (s > 0.25) s = 0.25;
    smooth = smooth > 0 ? smooth + (s - smooth) * 0.25 : s;
    float step = (float)((on_grid ? s : smooth) * 30);
    g_step = step < 0.1f ? 0.1f : step > 1.5f ? 1.5f : step;
    g_grid_streak = on_grid ? g_grid_streak + 1 : 0;
    static int fixed = -1;
    if (fixed < 0) fixed = env_int("LARECOMP_NO_CONTROLLER_STEP", 0);
    if (!fixed) *(float *)(uintptr_t)CONTROLLER_STEP = g_step;
}

// ------------------------------------------------------------------ the game's clock (ksys::VFR)
// VFR fields (1.8.2; the layout of zeldaret's ksys::VFR): +0x28 interval changed, +0x29 interval override on,
// +0x30 new interval ratio, +0x34 interval, +0x3c interval ratio.
#define VFR_CHANGED 0x28
#define VFR_OVERRIDE 0x29
#define VFR_NEW_RATIO 0x30
#define VFR_INTERVAL 0x34
#define VFR_RATIO 0x3c

void rt_vfr_update(uint64_t vfr) {
    if (g_game_version != 182 || !framepace_on()) return;
    static uint64_t last_t;
    static double real, given;  // seconds of real time and of game time handed out (only their gap matters)
    static double smooth;       // seconds between updates, averaged over about 20
    static double last_step;    // the step handed out last, seconds (0: none yet)
    static int last_target = -1;
    static double slow = 1;     // the step in console frames, averaged over about 20 (the interval follows it)
    static uint32_t last_iv;
    static int log_every = -1;
    static uint64_t nframe;
    if (log_every < 0) log_every = env_int("LARECOMP_LOG_FRAMEPACE", 0);
    uint64_t now = plat_now_ns();
    double s = last_t && now > last_t ? (now - last_t) / 1e9 : 1.0 / 30;
    last_t = now;
    float ratio;
    bool paused = *(uint32_t *)(uintptr_t)PAUSE_STATE != 0;
    if (paused) {
        // paused (it updates twice a frame): the frame's length between presents, nothing owed
        ratio = g_step;
        real = given = 0;
        last_target = -1;
    } else {
        if (s > 0.25) s = 0.25;  // (a load: not owed)
        double period = framepace_period_ns() / 1e9;
        int target = framepace_target();
        if (target != last_target) smooth = period, last_step = 0;  // (a new pace: start from it)
        last_target = target;
        smooth += (s - smooth) * 0.05;
        real += s;
        // The step changes slowly: the game reads some deltas a frame late, and a step that jumps between frames
        // moves the camera and Link apart. One period once the presents have kept the pace for half a second, else
        // the average of ~20 frames; plus a fiftieth of what is owed; and at most 3% away from the last step.
        double step = (g_grid_streak >= 30 ? period : smooth) + (real - given) * 0.02;
        if (last_step > 0 && step > last_step * 1.03) step = last_step * 1.03;
        if (last_step > 0 && step < last_step * 0.97) step = last_step * 0.97;
        if (step < 1.0 / FPS_CEILING) step = 1.0 / FPS_CEILING;
        if (step > 0.05) step = 0.05;
        last_step = step;
        ratio = (float)(step * 30);
    }
    bool held = VF(uint8_t, vfr, VFR_OVERRIDE) && ratio > 1;  // (stage loading: the game asks for single steps)
    if (held) ratio = 1;
    if (!paused) {
        given += ratio / 30.0;
        if (held || fabs(real - given) > 0.25) real = given;  // (nothing owed for a deliberate slow down)
    }
    // The interval switches some of the game's code between its 60 fps and 30 fps ways: only on a lasting change of
    // the frame rate, never back and forth from one frame to the next.
    slow += (ratio - slow) * 0.05;
    static uint32_t iv = 2;
    if (iv == 1 && slow > 0.85) iv = 2;
    else if (iv == 2 && slow < 0.7) iv = 1;
    else if (iv == 2 && slow > 1.3) iv = 3;
    else if (iv == 3 && slow < 1.15) iv = 2;
    VF(float, vfr, VFR_RATIO) = ratio;
    VF(float, vfr, VFR_NEW_RATIO) = ratio;
    VF(uint32_t, vfr, VFR_INTERVAL) = iv;
    VF(uint8_t, vfr, VFR_CHANGED) = last_iv && iv != last_iv;
    last_iv = iv;
    if (log_every > 0 && nframe++ % (uint64_t)log_every == 0)
        LOG("[framepace] target %d fps, %.2f ms since the last update (avg %.2f), ratio %.3f, interval %u, owed "
            "%.2f ms%s%s\n", framepace_target(), s * 1e3, smooth * 1e3, ratio, iv, (real - given) * 1e3,
            paused ? ", paused" : "", g_movie_playing > 0 ? ", movie" : "");
}
