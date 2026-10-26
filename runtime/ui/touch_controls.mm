// On-screen controls on Android (docs/las/ANDROID-PORT.md, Fase 4): both sticks, A / B / X / Y, L / R / ZL / ZR,
// L3 / R3 (the sticks' clicks), the d-pad, Minus / Plus and a button for the in-game menu, drawn over the game with
// ImGui (ui/menu.mm menu_render) while no controller is connected. Fingers come from SDL's touch events (ui_run,
// runtime/nvn/vulkan/device.c); what they hold joins the controller's buttons and sticks in runtime/hle/input.c
// (g_touch_buttons, g_touch_lx / ly, g_touch_rx / ry). Both sticks float: the left one starts where a finger lands in
// the left part of the screen, the right one (BOTW's camera) where it lands on the right, off the buttons. With the
// menu or the name entry up, touches stay mouse clicks for the menu. The Android back button opens the menu too.
// FPS-style options (Controls tab): the camera by dragging (drag_frame), the sticks drawn only while touched, and each
// stick's area a whole half of the screen.
// The layout is the player's (the Controls tab, ui/controls_ui.mm): each control's place (g_settings.touch_pos, a
// share of the screen; negative: the default), their size and opacity. Editing it (touch_edit_begin) closes the menu
// and lets the controls be dragged; a bar on the screen saves, resets or cancels.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "i18n.h"
#include "imgui.h"

extern "C" {
#include "settings.h"
#include "paths.h"
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
bool menu_input_captured(void);  // ui/menu.mm
void menu_test_open(void);       // ui/menu.mm: opens the in-game menu
void menu_request_close(void);   // ui/menu.mm
_Atomic uint64_t g_touch_buttons;  // (runtime/hle/input.c) Switch button bits held on the screen
_Atomic int32_t g_touch_lx, g_touch_ly;  // left stick, -32767..32767, y up
_Atomic int32_t g_touch_rx, g_touch_ry;  // right stick
}

// the Switch button bits (runtime/hle/input.c)
enum { TB_A = 0, TB_B, TB_X, TB_Y, TB_LSTICK, TB_RSTICK, TB_L, TB_R, TB_ZL, TB_ZR, TB_PLUS, TB_MINUS,
       TB_LEFT, TB_UP, TB_RIGHT, TB_DOWN };
enum { C_NONE = -1, C_STICK = 100, C_MENU = 101, C_DONE = 102, C_RESET = 103, C_CANCEL = 104, C_RSTICK = 105 };
// the controls, in g_settings.touch_pos order (TOUCH_NCTL entries; LAS's first twelve keep their places in saved layouts)
enum { K_A, K_B, K_X, K_Y, K_L, K_ZL, K_R, K_ZR, K_MINUS, K_PLUS, K_MENU, K_STICK,
       K_RSTICK, K_LS, K_RS, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_N };
_Static_assert(K_N == TOUCH_NCTL, "settings.h TOUCH_NCTL");

typedef struct { float x, y, r; int bit; const char *label; bool rect; float w, h; int k; } Ctl;

static pthread_mutex_t t_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { SDL_FingerID id; int ctl; float x, y, dx, dy; } fingers[10];
static int nfingers;
// the sticks (0 left, 1 right): where each started and where its knob is (pixels)
static struct { bool on; float cx, cy, kx, ky; } stick[2];
static float disp_w = 1, disp_h = 1;  // the window in pixels (as last drawn)
static atomic_bool used;  // a touch since the last controller: show the controls
static atomic_bool hide_auto;  // a controller is connected (as of the last event or check)
static atomic_bool editing;
static float edit_pos[K_N][2];  // (editing) the layout being edited
static _Atomic uint64_t last_draw_ms;

static void default_pos(int k, float *x, float *y) {  // in pixels
    float w = disp_w, h = disp_h, u = h / 100.0f, gap = 13.0f * u, fx = w - 20 * u, fy = h - 27 * u, sw = 15 * u;
    float dx = 15 * u, dy = h - 58 * u, dg = 7.5f * u;  // the d-pad: above the left stick
    switch (k) {
    case K_A: *x = fx + gap, *y = fy; break;  // face buttons: a diamond, Switch layout (A east)
    case K_B: *x = fx, *y = fy + gap; break;
    case K_X: *x = fx, *y = fy - gap; break;
    case K_Y: *x = fx - gap, *y = fy; break;
    case K_L: *x = 4 * u + sw / 2, *y = 20 * u; break;  // shoulders: L / ZL top left, R / ZR top right
    case K_ZL: *x = 4 * u + sw / 2, *y = 10 * u; break;
    case K_R: *x = w - 4 * u - sw / 2, *y = 20 * u; break;
    case K_ZR: *x = w - 4 * u - sw / 2, *y = 10 * u; break;
    case K_MINUS: *x = w / 2 - 9 * u, *y = h - 7 * u; break;
    case K_PLUS: *x = w / 2 + 9 * u, *y = h - 7 * u; break;
    case K_MENU: *x = w / 2, *y = 6 * u; break;
    case K_RSTICK: *x = w - 62 * u, *y = h - 27 * u; break;  // the right stick's resting place
    case K_LS: *x = 40 * u, *y = h - 8 * u; break;  // the sticks' clicks, under each stick's resting place
    case K_RS: *x = w - 50 * u, *y = h - 8 * u; break;
    case K_UP: *x = dx, *y = dy - dg; break;
    case K_DOWN: *x = dx, *y = dy + dg; break;
    case K_LEFT: *x = dx - dg, *y = dy; break;
    case K_RIGHT: *x = dx + dg, *y = dy; break;
    default: *x = 20 * u, *y = h - 27 * u; break;  // the left stick's resting place
    }
}

static void pos_of(int k, float *x, float *y) {
    const float *p = editing ? edit_pos[k] : g_settings.touch_pos[k];
    if (p[0] >= 0 && p[1] >= 0) *x = p[0] * disp_w, *y = p[1] * disp_h;
    else default_pos(k, x, y);
}

static float size_scale(void) { return g_settings.touch_scale > 0.2f ? g_settings.touch_scale : 1.0f; }
static float stick_radius(void) { return disp_h * 0.13f * size_scale(); }

static int layout(Ctl *c) {  // in pixels of the window (landscape)
    float u = disp_h / 100.0f, z = size_scale();
    float br = 7.5f * u * z, sw = 15 * u * z, sh = 7 * u * z, sr = 4.5f * u * z, dr = 3.8f * u * z;
    static const struct { int k, bit; const char *label; int shape; } T[] = {
        {K_A, TB_A, "A", 0}, {K_B, TB_B, "B", 0}, {K_X, TB_X, "X", 0}, {K_Y, TB_Y, "Y", 0},
        {K_L, TB_L, "L", 1}, {K_ZL, TB_ZL, "ZL", 1}, {K_R, TB_R, "R", 1}, {K_ZR, TB_ZR, "ZR", 1},
        {K_MINUS, TB_MINUS, "-", 2}, {K_PLUS, TB_PLUS, "+", 2}, {K_MENU, C_MENU, "=", 2},
        {K_LS, TB_LSTICK, "L3", 2}, {K_RS, TB_RSTICK, "R3", 2},
        {K_UP, TB_UP, "^", 3}, {K_DOWN, TB_DOWN, "v", 3}, {K_LEFT, TB_LEFT, "<", 3}, {K_RIGHT, TB_RIGHT, ">", 3}};
    int n = 0;
    for (const auto &t : T) {
        float x, y;
        pos_of(t.k, &x, &y);
        c[n++] = t.shape == 1 ? (Ctl){x, y, 0, t.bit, t.label, true, sw, sh, t.k}
                              : (Ctl){x, y, t.shape == 2 ? sr : t.shape == 3 ? dr : br, t.bit, t.label, false, 0, 0, t.k};
    }
    if (editing) {  // the bar: save, defaults, cancel
        float bw = 16 * u, bh = 8 * u, y = 30 * u;
        c[n++] = (Ctl){disp_w / 2 - 18 * u, y, 0, C_DONE, "Done", true, bw, bh, -1};
        c[n++] = (Ctl){disp_w / 2, y, 0, C_RESET, "Default", true, bw, bh, -1};
        c[n++] = (Ctl){disp_w / 2 + 18 * u, y, 0, C_CANCEL, "Cancel", true, bw, bh, -1};
    }
    return n;
}

static bool in_ctl(const Ctl *c, float x, float y) {
    float u = disp_h / 100.0f;
    if (c->rect) return fabsf(x - c->x) <= c->w / 2 + u && fabsf(y - c->y) <= c->h / 2 + u;
    float dx = x - c->x, dy = y - c->y, r = c->r * 1.25f;  // (a bit larger than drawn: thumbs)
    return dx * dx + dy * dy <= r * r;
}

static int hit(float x, float y, int *k) {  // the control under (x, y) (*k: its layout entry)
    Ctl c[32];
    int n = layout(c);
    for (int i = n; i-- > 0;)  // (the bar first)
        if (in_ctl(&c[i], x, y)) { *k = c[i].k; return c[i].bit; }
    float R = stick_radius();
    if (editing) {  // the sticks' resting places can be dragged
        for (int s = 0; s < 2; s++) {
            float sx, sy;
            pos_of(s ? K_RSTICK : K_STICK, &sx, &sy);
            if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= R * R) { *k = s ? K_RSTICK : K_STICK; return s ? C_RSTICK : C_STICK; }
        }
        *k = -1;
        return C_NONE;
    }
    // (Larger areas: each a whole half of the screen, the buttons still in front; the drag camera has no stick of its own
    // to stay near, so it takes the whole right half too)
    bool half = g_settings.touch_half_areas, right_half = half || g_settings.touch_drag_camera;
    *k = K_STICK;
    if (half ? x < disp_w * 0.5f : x < disp_w * 0.45f && y > disp_h * 0.30f) return C_STICK;
    *k = K_RSTICK;
    if (right_half ? x >= disp_w * 0.5f : x > disp_w * 0.50f && y > disp_h * 0.25f) return C_RSTICK;  // (the camera)
    *k = -1;
    return C_NONE;
}

static void stick_update(int s) {  // (t_lock held)
    float R = stick_radius();
    float dx = stick[s].kx - stick[s].cx, dy = stick[s].ky - stick[s].cy, d = sqrtf(dx * dx + dy * dy);
    if (d > R) { dx *= R / d, dy *= R / d; stick[s].kx = stick[s].cx + dx, stick[s].ky = stick[s].cy + dy; }
    int32_t x = (int32_t)(dx / R * 32767), y = (int32_t)(-dy / R * 32767);
    if (s) g_touch_rx = x, g_touch_ry = y;
    else g_touch_lx = x, g_touch_ly = y;
}

// The camera by dragging (g_settings.touch_drag_camera; FPS-style, a Reddit request): the finger's speed, not where it
// is, tilts the right stick - the camera turns while the finger moves and stops with it. Its travel since the last frame
// (drag_dx / dy, pixels) becomes the tilt once a frame (drag_frame): a full tilt at 80% of the screen's height a second
// (times touch_drag_sens), a little smoothed (touch events and frames come at different rates), and past the stick's
// first 15% (the game's dead zone: slow drags would turn nothing).
static float drag_dx, drag_dy, drag_vx, drag_vy;
static uint64_t drag_last_ns;

static bool drag_camera(void) { return g_settings.touch_drag_camera && !editing; }

static void drag_frame(void) {  // (t_lock held) once a presented frame
    uint64_t now = SDL_GetTicksNS();
    double dt = drag_last_ns ? (double)(now - drag_last_ns) / 1e9 : 0;
    drag_last_ns = now;
    if (!drag_camera() || !stick[1].on) { drag_vx = drag_vy = drag_dx = drag_dy = 0; return; }
    if (dt <= 0 || dt > 0.25) dt = 1.0 / 30;
    float sens = g_settings.touch_drag_sens > 0.1f ? g_settings.touch_drag_sens : 1.0f;
    float full = disp_h * 0.8f / sens;  // (pixels a second for a full tilt)
    float vx = (float)(drag_dx / dt) / full, vy = (float)(-drag_dy / dt) / full;
    drag_dx = drag_dy = 0;
    drag_vx = 0.5f * drag_vx + 0.5f * vx, drag_vy = 0.5f * drag_vy + 0.5f * vy;
    float m = sqrtf(drag_vx * drag_vx + drag_vy * drag_vy), out = m < 0.01f ? 0 : fminf(1.0f, 0.15f + 0.85f * m);
    float k = m > 0 ? out / m : 0;
    g_touch_rx = (int32_t)(drag_vx * k * 32767), g_touch_ry = (int32_t)(drag_vy * k * 32767);
}

static void stick_release(int s) {  // (t_lock held)
    stick[s].on = false;
    if (s) g_touch_rx = 0, g_touch_ry = 0, drag_dx = drag_dy = drag_vx = drag_vy = 0;
    else g_touch_lx = 0, g_touch_ly = 0;
}

static void recompute(void) {  // (t_lock held)
    uint64_t b = 0;
    for (int i = 0; i < nfingers && !editing; i++)
        if (fingers[i].ctl >= 0 && fingers[i].ctl < 16) b |= 1ull << fingers[i].ctl;
    g_touch_buttons = b;
}

static void release_all(void) {
    pthread_mutex_lock(&t_lock);
    nfingers = 0;
    stick_release(0);
    stick_release(1);
    g_touch_buttons = 0;
    pthread_mutex_unlock(&t_lock);
}

// ---- layout editing (the Controls tab)
extern "C" bool touch_edit_available(void) {  // the overlay is up (in game): there is something to drag
    return SDL_GetTicks() - last_draw_ms < 500 && g_settings.touch_controls;
}

extern "C" void touch_edit_begin(void) {
    memcpy(edit_pos, g_settings.touch_pos, sizeof edit_pos);
    release_all();
    editing = true;
    used = true;
    menu_request_close();
}

static void edit_end(int how) {  // C_DONE / C_RESET / C_CANCEL
    if (how == C_RESET) {
        for (int k = 0; k < K_N; k++) edit_pos[k][0] = edit_pos[k][1] = -1;
        return;  // (still editing: the defaults on the screen)
    }
    if (how == C_DONE) {
        memcpy(g_settings.touch_pos, edit_pos, sizeof edit_pos);
        settings_save();
    }
    release_all();
    editing = false;
}

extern "C" void touch_layout_reset(void) {
    for (int k = 0; k < K_N; k++) g_settings.touch_pos[k][0] = g_settings.touch_pos[k][1] = -1;
    settings_save();
}

extern "C" bool touch_controls_visible(void) {
    // a controller connected before the game started (its ADDED event went to the start screen): checked here too,
    // a few times a second
    static _Atomic uint64_t checked_ms;
    uint64_t now = SDL_GetTicks();
    if (now - checked_ms > 250) {
        checked_ms = now;
        bool pad = SDL_HasGamepad();
        if (pad && !hide_auto) release_all();
        hide_auto = pad;
    }
    return editing || (!hide_auto && !menu_input_captured() && g_settings.touch_controls);
}

// ---- the performance overlay (ui/perf_overlay.mm), dragged with a finger anywhere on the screen - with or without the
// on-screen controls (a controller connected) - and kept there (g_settings.overlay_pos); LASRecompAndroid's
extern float g_perf_rect[4];  // where it was last drawn (pixels)
static SDL_FingerID overlay_finger;
static bool overlay_dragging;
static float overlay_dx, overlay_dy;

static bool overlay_drag(const SDL_Event *e, float x, float y) {  // true: the finger is the overlay's
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        const float *r = g_perf_rect;
        if (overlay_dragging || !g_settings.show_fps || r[2] <= 0 || x < r[0] || x > r[0] + r[2] || y < r[1] || y > r[1] + r[3])
            return false;
        overlay_dragging = true, overlay_finger = e->tfinger.fingerID, overlay_dx = r[0] - x, overlay_dy = r[1] - y;
        return true;
    }
    if (!overlay_dragging || e->tfinger.fingerID != overlay_finger) return false;
    float nx = (x + overlay_dx) / disp_w, ny = (y + overlay_dy) / disp_h;  // (perf_overlay.mm keeps it on the screen)
    g_settings.overlay_pos[0] = nx < 0 ? 0 : nx > 1 ? 1 : nx;
    g_settings.overlay_pos[1] = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) overlay_dragging = false, settings_save();
    return true;
}

// SDL events (the UI thread): true when the event was for the on-screen controls.
extern "C" bool touch_controls_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: hide_auto = true; release_all(); return false;
    case SDL_EVENT_GAMEPAD_REMOVED: hide_auto = SDL_HasGamepad(); return false;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_AC_BACK && !e->key.repeat) {
            if (editing) { edit_end(C_CANCEL); return true; }
            if (!menu_input_captured()) { menu_test_open(); return true; }
        }
        return false;
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED: break;
    default: return false;
    }
    if (!editing && !menu_input_captured() && overlay_drag(e, e->tfinger.x * disp_w, e->tfinger.y * disp_h)) return true;
    if (!editing && (menu_input_captured() || !g_settings.touch_controls || hide_auto)) { release_all(); return false; }  // (the menu's clicks)
    used = true;
    float x = e->tfinger.x * disp_w, y = e->tfinger.y * disp_h;
    pthread_mutex_lock(&t_lock);
    int f = -1;
    for (int i = 0; i < nfingers; i++) if (fingers[i].id == e->tfinger.fingerID) f = i;
    int act = C_NONE;  // (after the lock: menu / the edit bar)
    if (e->type == SDL_EVENT_FINGER_DOWN && f < 0 && nfingers < 10) {
        int k, ctl = hit(x, y, &k);
        int s = ctl == C_STICK ? 0 : ctl == C_RSTICK ? 1 : -1;
        if (s >= 0 && stick[s].on && !editing) ctl = C_NONE, s = -1;  // (one finger per stick)
        float cx, cy;
        if (k >= 0) pos_of(k, &cx, &cy); else cx = x, cy = y;
        fingers[nfingers] = {e->tfinger.fingerID, ctl, x, y, cx - x, cy - y};
        if (editing && k >= 0 && ctl != C_NONE && (ctl < C_DONE || ctl == C_RSTICK)) fingers[nfingers].ctl = 200 + k;  // (dragging layout entry k)
        nfingers++;
        if (!editing && s >= 0) {
            stick[s].on = true;
            stick[s].cx = stick[s].kx = x, stick[s].cy = stick[s].ky = y;
            if (s == 1 && drag_camera()) drag_dx = drag_dy = drag_vx = drag_vy = 0;
            else stick_update(s);
        }
        if (ctl == C_MENU && !editing) act = C_MENU;
        if (ctl >= C_DONE && ctl <= C_CANCEL) act = ctl;
    } else if (e->type == SDL_EVENT_FINGER_MOTION && f >= 0) {
        float px = fingers[f].x, py = fingers[f].y;
        fingers[f].x = x, fingers[f].y = y;
        if (fingers[f].ctl == C_RSTICK && drag_camera()) {  // (its travel: drag_frame turns it into the tilt)
            drag_dx += x - px, drag_dy += y - py;
            stick[1].kx = x, stick[1].ky = y;
        } else if (fingers[f].ctl >= 200) {  // (editing) the control follows the finger, kept on the screen
            int k = fingers[f].ctl - 200;
            float nx = (x + fingers[f].dx) / disp_w, ny = (y + fingers[f].dy) / disp_h;
            edit_pos[k][0] = nx < 0.02f ? 0.02f : nx > 0.98f ? 0.98f : nx;
            edit_pos[k][1] = ny < 0.03f ? 0.03f : ny > 0.97f ? 0.97f : ny;
        } else if ((fingers[f].ctl == C_STICK || fingers[f].ctl == C_RSTICK) && !editing) {
            int s = fingers[f].ctl == C_RSTICK;
            stick[s].kx = x, stick[s].ky = y;
            stick_update(s);
        }
    } else if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) && f >= 0) {
        if ((fingers[f].ctl == C_STICK || fingers[f].ctl == C_RSTICK) && !editing) stick_release(fingers[f].ctl == C_RSTICK);
        fingers[f] = fingers[--nfingers];
    }
    recompute();
    pthread_mutex_unlock(&t_lock);
    if (act == C_MENU) { release_all(); menu_test_open(); }
    else if (act >= C_DONE) edit_end(act);
    return true;
}

// One persistent atlas in the menu's ImGui context (the context survives game reboots).
// Transparent frame holes let the existing fill and opacity settings show through.
static ImTextureID touch_skin;
static void load_touch_skin(void) {
    static bool attempted;
    if (attempted) return;
    attempted = true;
    char path[1100];
    if (!rt_resource("sheikah-touch-atlas.png", "assets/artwork/sheikah-touch-atlas-v1.png", path, sizeof path)) return;
    SDL_Surface *source = SDL_LoadPNG(path);
    SDL_Surface *rgba = source ? SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (source) SDL_DestroySurface(source);
    if (!rgba) return;
    std::vector<uint8_t> pixels((size_t)rgba->w * rgba->h * 4);
    for (int y = 0; y < rgba->h; y++)
        memcpy(pixels.data() + (size_t)y * rgba->w * 4,
               (const uint8_t *)rgba->pixels + (size_t)y * rgba->pitch, (size_t)rgba->w * 4);
    touch_skin = (ImTextureID)vk_imgui_texture_rgba(pixels.data(), rgba->w, rgba->h);
    if (touch_skin) fprintf(stderr, "[touch] Sheikah skin loaded (%dx%d)\n", rgba->w, rgba->h);
    SDL_DestroySurface(rgba);
}

enum { SKIN_BUTTON, SKIN_SHOULDER, SKIN_STICK, SKIN_KNOB };
static bool skin_image(ImDrawList *dl, int sprite, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (!touch_skin) return false;
    // Pixel bounds of the four independent drawings in the original 1254-square atlas.
    static const float bounds[4][4] = {{24, 26, 602, 562}, {633, 185, 1247, 412},
                                      {22, 617, 606, 1187}, {706, 677, 1175, 1138}};
    const float *uv = bounds[sprite];
    dl->AddImage(touch_skin, a, b, ImVec2(uv[0] / 1254, uv[1] / 1254),
                 ImVec2(uv[2] / 1254, uv[3] / 1254), tint);
    return true;
}

static void sheikah_eye(ImDrawList *dl, ImVec2 p, float r, ImU32 color) {
    float w = r * 0.55f, h = r * 0.22f, stroke = fmaxf(1, r * 0.06f);
    dl->PathLineTo(ImVec2(p.x - w, p.y));
    dl->PathBezierCubicCurveTo(ImVec2(p.x - w * 0.35f, p.y - h * 1.8f),
                              ImVec2(p.x + w * 0.35f, p.y - h * 1.8f), ImVec2(p.x + w, p.y));
    dl->PathBezierCubicCurveTo(ImVec2(p.x + w * 0.35f, p.y + h * 1.8f),
                              ImVec2(p.x - w * 0.35f, p.y + h * 1.8f), ImVec2(p.x - w, p.y));
    dl->PathStroke(color, ImDrawFlags_Closed, stroke);
    dl->AddCircle(p, r * 0.15f, color, 16, stroke);
    for (int i = -1; i <= 1; i++) {
        float x = p.x + i * r * 0.28f, y = p.y - r * 0.38f;
        dl->AddTriangleFilled(ImVec2(x - r * 0.08f, y), ImVec2(x + r * 0.08f, y),
                              ImVec2(x, y - r * 0.18f), color);
    }
    dl->AddTriangleFilled(ImVec2(p.x - r * 0.09f, p.y + h), ImVec2(p.x + r * 0.09f, p.y + h),
                          ImVec2(p.x, p.y + r * 0.66f), color);
}

// Drawn with the in-game overlay (ui/menu.mm menu_render), between ImGui's NewFrame and Render.
extern "C" void touch_controls_draw(ImVec2 display, ImFont *font) {
    disp_w = display.x, disp_h = display.y;
    last_draw_ms = SDL_GetTicks();
    pthread_mutex_lock(&t_lock);
    drag_frame();
    pthread_mutex_unlock(&t_lock);
    if (!touch_controls_visible()) return;
    load_touch_skin();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    Ctl c[32];
    int n = layout(c);
    uint64_t held = g_touch_buttons;
    float u = disp_h / 100.0f;
    float op = g_settings.touch_opacity > 0.05f ? g_settings.touch_opacity : 1.0f;
    float alpha = (used || editing ? 1.0f : 0.6f) * op;
    // Alpha values and idle fading are unchanged from the original touch controls.
    ImU32 fill = IM_COL32(20, 30, 33, (int)(40 * alpha)), fill_on = IM_COL32(45, 205, 235, (int)(120 * op));
    ImU32 line = IM_COL32(255, 255, 255, (int)(110 * alpha)), text = IM_COL32(240, 235, 219, (int)(200 * alpha));
    if (editing) {
        dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 90));
        const char *tip = tr("Drag the controls to move them");
        float fs = 4.5f * u;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, tip);
        dl->AddText(font, fs, ImVec2(disp_w / 2 - ts.x / 2, 20 * u), IM_COL32(255, 255, 255, 230), tip);
    }
    for (int i = 0; i < n; i++) {
        bool bar = c[i].bit >= C_DONE && c[i].bit <= C_CANCEL;
        bool on = c[i].bit >= 0 && c[i].bit < 16 && (held >> c[i].bit & 1);
        float fs = (c[i].rect ? c[i].h * (bar ? 0.45f : 0.48f) : c[i].r * 0.9f);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, c[i].label);
        ImU32 f = bar ? IM_COL32(60, 90, 70, 230) : on ? fill_on : fill, l = bar ? IM_COL32(236, 194, 84, 255) : line;
        ImU32 tx = bar ? IM_COL32(255, 255, 255, 255) : text;
        if (c[i].rect) {
            ImVec2 a(c[i].x - c[i].w / 2, c[i].y - c[i].h / 2), b(c[i].x + c[i].w / 2, c[i].y + c[i].h / 2);
            dl->AddRectFilled(a, b, f, 1.5f * u);
            if (bar || !skin_image(dl, SKIN_SHOULDER, a, b, l))
                dl->AddRect(a, b, l, 1.5f * u, 0, 0.3f * u);
        } else {
            dl->AddCircleFilled(ImVec2(c[i].x, c[i].y), c[i].r, f, 32);
            ImVec2 a(c[i].x - c[i].r, c[i].y - c[i].r), b(c[i].x + c[i].r, c[i].y + c[i].r);
            if (!skin_image(dl, SKIN_BUTTON, a, b, l))
                dl->AddCircle(ImVec2(c[i].x, c[i].y), c[i].r, l, 32, 0.3f * u);
        }
        if (c[i].bit == C_MENU) {
            sheikah_eye(dl, ImVec2(c[i].x, c[i].y), c[i].r, IM_COL32(66, 215, 245, (int)(200 * alpha)));
        } else if (c[i].bit >= TB_LEFT && c[i].bit <= TB_DOWN) {  // d-pad: a triangle pointing its way
            static const float dir[4][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}};  // LEFT, UP, RIGHT, DOWN
            const float *d = dir[c[i].bit - TB_LEFT];
            float r = c[i].r * 0.5f, px = -d[1], py = d[0];
            ImVec2 tip(c[i].x + d[0] * r, c[i].y + d[1] * r);
            ImVec2 b1(c[i].x - d[0] * r * 0.6f + px * r * 0.8f, c[i].y - d[1] * r * 0.6f + py * r * 0.8f);
            ImVec2 b2(c[i].x - d[0] * r * 0.6f - px * r * 0.8f, c[i].y - d[1] * r * 0.6f - py * r * 0.8f);
            dl->AddTriangleFilled(tip, b1, b2, tx);
        } else {
            dl->AddText(font, fs, ImVec2(c[i].x - ts.x / 2, c[i].y - ts.y / 2), tx, c[i].label);
        }
    }
    // the sticks: where each started (or its resting place) and the knob - none for the drag camera, and only while a
    // finger holds them with "only while touched" (but always while editing the layout: they're dragged into place)
    float R = stick_radius();
    for (int s = 0; s < 2; s++) {
        if (s == 1 && drag_camera()) continue;
        pthread_mutex_lock(&t_lock);
        bool on = stick[s].on;
        if (g_settings.touch_sticks_hidden && !on && !editing) { pthread_mutex_unlock(&t_lock); continue; }
        float rx, ry;
        pos_of(s ? K_RSTICK : K_STICK, &rx, &ry);
        float cx = on ? stick[s].cx : rx, cy = on ? stick[s].cy : ry, kx = on ? stick[s].kx : cx, ky = on ? stick[s].ky : cy;
        pthread_mutex_unlock(&t_lock);
        dl->AddCircleFilled(ImVec2(cx, cy), R, fill, 48);
        if (!skin_image(dl, SKIN_STICK, ImVec2(cx - R, cy - R), ImVec2(cx + R, cy + R), line))
            dl->AddCircle(ImVec2(cx, cy), R, line, 48, 0.3f * u);
        float kr = R * 0.45f;
        ImU32 knob = IM_COL32(255, 255, 255, (int)(on ? 120 * op : 70 * alpha));
        if (!skin_image(dl, SKIN_KNOB, ImVec2(kx - kr, ky - kr), ImVec2(kx + kr, ky + kr), knob))
            dl->AddCircleFilled(ImVec2(kx, ky), kr, on ? fill_on : IM_COL32(20, 30, 33, (int)(70 * alpha)), 32);
    }
}
#endif
