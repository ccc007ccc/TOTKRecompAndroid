// NVN compares the counter fields of two adjacent NVNcounterData records.
// The timestamp fields at +8 and +24 do not participate in the comparison.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool nvn_render_condition(const void *records, uint32_t mode, uint64_t *a, uint64_t *b) {
    memcpy(a, records, sizeof *a);
    memcpy(b, (const uint8_t *)records + 16, sizeof *b);
    return mode == 0 ? *a == *b : *a != *b;
}
