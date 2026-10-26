#pragma once
#include <stdint.h>

// Application-manager cache creation and FSP mounting share the same persisted catalog.
uint32_t fs_create_cache_storage(uint16_t index, int64_t size, int64_t journal);
