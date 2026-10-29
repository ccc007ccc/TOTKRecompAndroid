#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diagnostics_build.h"

// Optional developer boot flow; inputs are read from the player's own RomFS.
#if TOTK_DIAGNOSTICS
bool test_boot_target(const char *path);
// 1: patched, 0: unrelated file, -1: unsupported input (left unchanged).
int test_boot_patch(const char *path, uint8_t *data, size_t size, int slot);
#else
static inline bool test_boot_target(const char *path) { (void)path; return false; }
static inline int test_boot_patch(const char *path, uint8_t *data, size_t size, int slot) {
    (void)path; (void)data; (void)size; (void)slot; return 0;
}
#endif
