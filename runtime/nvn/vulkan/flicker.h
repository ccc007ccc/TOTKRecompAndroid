#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../diagnostics_build.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool active, saving, single_pending, available, full;
    unsigned saved, skipped;
    float threshold;
    char directory[768], error[160];
} FlickerStatus;
#if TOTK_DIAGNOSTICS
void flicker_start(void);
void flicker_stop(void);
void flicker_single(void);
void flicker_next_frame(bool visible); // render thread, after the previous frame was submitted
void flicker_set_threshold(float threshold);
FlickerStatus flicker_status(void);
void flicker_reset(void); // GPU idle, during game teardown
#else
static inline void flicker_start(void) {}
static inline void flicker_stop(void) {}
static inline void flicker_single(void) {}
static inline void flicker_next_frame(bool visible) { (void)visible; }
static inline void flicker_set_threshold(float threshold) { (void)threshold; }
static inline FlickerStatus flicker_status(void) { FlickerStatus status = {0}; return status; }
static inline void flicker_reset(void) {}
#endif
#ifdef __cplusplus
}
#endif
