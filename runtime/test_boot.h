#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Optional developer boot flow; inputs are read from the player's own RomFS.
bool test_boot_target(const char *path);
// 1: patched, 0: unrelated file, -1: unsupported input (left unchanged).
int test_boot_patch(const char *path, uint8_t *data, size_t size, int slot);
