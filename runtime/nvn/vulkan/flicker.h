#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool active, saving, single_pending, available, full;
    unsigned saved, skipped;
    float threshold;
    char directory[768], error[160];
} FlickerStatus;
void flicker_start(void);
void flicker_stop(void);
void flicker_single(void);
void flicker_next_frame(bool visible); // render thread, after the previous frame was submitted
void flicker_set_threshold(float threshold);
FlickerStatus flicker_status(void);
void flicker_reset(void); // GPU idle, during game teardown
#ifdef __cplusplus
}
#endif
