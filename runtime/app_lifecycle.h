#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
// Separate from test pause: Android's SDL event thread can be blocked while
// guest/render threads keep running. Publish focus before SDL blocks that thread.
void app_lifecycle_set_foreground(bool foreground);
bool app_lifecycle_backgrounded(void);
void app_lifecycle_wait(void); // guest frame boundary, no renderer/object locks held
void app_lifecycle_stop(void); // release waiters before joining guest threads
void app_lifecycle_start(void); // new guest boot; preserve actual activity focus
#ifdef __cplusplus
}
#endif
