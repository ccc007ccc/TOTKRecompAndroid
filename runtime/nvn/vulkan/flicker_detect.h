// Pure CPU event selection; retained images are before / peak / after, not an arbitrary last-three dump.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

static float flicker_difference(const uint8_t *a, const uint8_t *b, unsigned w, unsigned h) {
    float sum[144] = {0}; unsigned count[144] = {0};
    float total = 0; unsigned n = 0;
    // Sample every fourth pixel. The source is the game image, before touch/menu/FPS overlays.
    for (unsigned y = 2; y < h; y += 4) for (unsigned x = 2; x < w; x += 4) {
        unsigned p = (y * w + x) * 4, tile = (y * 9 / h) * 16 + x * 16 / w;
        float d = (fabsf((float)a[p] - b[p]) + fabsf((float)a[p+1] - b[p+1]) +
                   fabsf((float)a[p+2] - b[p+2])) / 765.0f;
        sum[tile] += d; count[tile]++; total += d; n++;
    }
    float first = 0, second = 0;
    for (unsigned i = 0; i < 144; i++) {
        float d = count[i] ? sum[i] / count[i] : 0;
        if (d > first) { second = first; first = d; } else if (d > second) second = d;
    }
    // Local changes must cover at least two tiles; small HUD/text changes contribute little.
    return fmaxf(n ? total / n : 0, second * 0.55f);
}

typedef struct {
    int before, peak;
    unsigned stable;
    float strength, previous_strength;
    uint64_t start_ns, cooldown_ns;
} FlickerDetector;

enum { FLICKER_CONTINUE, FLICKER_RECOVERED, FLICKER_SETTLED, FLICKER_TIMEOUT };
static void flicker_detector_clear(FlickerDetector *d) {
    *d = (FlickerDetector){.before = -1, .peak = -1};
}
static bool flicker_detector_begin(FlickerDetector *d, int before, int peak, float change,
                                   float threshold, uint64_t ns) {
    if (ns < d->cooldown_ns || change < threshold) return false;
    d->before = before; d->peak = peak; d->strength = d->previous_strength = change;
    d->start_ns = ns; d->stable = 0;
    return true;
}
// Caller exchanges the peak image when this returns true, preserving exactly three snapshot slots.
static bool flicker_new_peak(FlickerDetector *d, float change) {
    if (change <= d->strength * 1.05f) return false;
    d->strength = change; d->stable = 0;
    return true;
}
static int flicker_detector_after(FlickerDetector *d, float from_before, float from_peak,
                                  float threshold, uint64_t ns) {
    if (from_before <= fmaxf(threshold * 0.35f, d->strength * 0.3f)) return FLICKER_RECOVERED;
    if (from_peak < threshold * 0.2f && fabsf(from_before - d->previous_strength) < threshold * 0.2f)
        d->stable++;
    else d->stable = 0;
    d->previous_strength = from_before;
    if (d->stable >= 3) return FLICKER_SETTLED;
    return ns - d->start_ns >= 1500000000ull ? FLICKER_TIMEOUT : FLICKER_CONTINUE;
}
