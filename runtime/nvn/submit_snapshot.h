#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Android submit-time data capture, adapted from the MIT MacNativeGames nvn_mtl.m mechanism.
// All capture calls are serialized with the async producer lock. Replay/write/reset calls run on the render thread
// (reset requires a drained queue). The returned byte pointers belong to the current request, never to GPU commands.
typedef struct SubmitSnapshot SubmitSnapshot;
typedef bool (*SubmitRead)(uint64_t addr, uint64_t size, void *dst, uint64_t *pool_lo, uint64_t *pool_hi);

#ifdef __ANDROID__
bool submit_snapshot_enabled(void);
SubmitSnapshot *submit_snapshot_capture(const void *list, size_t bytes, uint64_t retired, SubmitRead read, bool *sync);
void submit_snapshot_begin(SubmitSnapshot *snapshot);
void submit_snapshot_end(void);
const void *submit_snapshot_data(uint64_t addr, uint64_t size);
void submit_snapshot_copy(uint64_t addr, uint64_t size, void *dst);
void submit_snapshot_write(uint64_t addr, uint64_t size, uint64_t seq);
void submit_snapshot_free(SubmitSnapshot *snapshot);
void submit_snapshot_reset(void);
uint64_t submit_snapshot_inflight_bytes(void);
bool nvn_submit_read(uint64_t addr, uint64_t size, void *dst, uint64_t *pool_lo, uint64_t *pool_hi);
#endif
