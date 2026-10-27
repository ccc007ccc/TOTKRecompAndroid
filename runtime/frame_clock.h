// Present durations for the TotK time producer. No lower step clamp: clipping short frames at 1/120 s makes
// jitter around 120 FPS advance too much game time. Long pauses discard the sampling history, not catch up.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t previous, period;
    double samples[5];
    unsigned count, next;
} FrameClock;

static inline double frame_clock_update(FrameClock *clock, uint64_t now, uint64_t period) {
    double nominal = period * 1e-9;
    if (nominal > 1.0 / 15.0) nominal = 1.0 / 15.0;
    if (!clock->previous || now <= clock->previous || now - clock->previous > 250000000 || clock->period != period) {
        *clock = (FrameClock){.previous = now, .period = period};
        return nominal;
    }
    double elapsed = (now - clock->previous) * 1e-9;
    clock->previous = now;
    if (elapsed > 1.0 / 15.0) elapsed = 1.0 / 15.0;
    clock->samples[clock->next++ % 5] = elapsed;
    if (clock->count < 5) clock->count++;
    double total = 0;
    for (unsigned i = 0; i < clock->count; i++) total += clock->samples[i];
    return total / clock->count;
}
