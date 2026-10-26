#pragma once
#include <stdbool.h>
#include <stdint.h>

// A cached snapshot belongs to one allocation, not merely to the NVN object pointing at that allocation.
// The generation also rejects a newly allocated image which reuses an old Vulkan handle (ABA).
static bool feedback_identity_matches(uint64_t saved_image, uint64_t saved_generation,
                                       uint64_t image, uint64_t generation) {
    return saved_image && saved_image == image && saved_generation == generation;
}
