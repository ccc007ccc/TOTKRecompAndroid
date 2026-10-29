#include "test_boot.h"
#include <string.h>

static const char load[] = "Sequence/OnLoadGameSaveData.module.ainb";

bool test_boot_target(const char *path) {
    return !strcmp(path, load);
}

static uint64_t fingerprint(const uint8_t *data, size_t size) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

static void put32(uint8_t *data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[i] = (uint8_t)(value >> (8*i));
}

int test_boot_patch(const char *path, uint8_t *data, size_t size, int slot) {
    if (!test_boot_target(path)) return 0;
    if (!data || slot < 0 || slot > 5) return -1;
    {
        if (size != 7845 || fingerprint(data, size) != UINT64_C(0x4a1c97cdcae85f0f)) return -1;
        // AINB 0x407, TotK 1.4.3. Only change the four Index input connections:
        // load types 0/6/5 and SetCaptionMgrLatestIndex. Keep every load/reset,
        // scene setup and after-load action, as well as all unrelated bytes.
        static const struct { unsigned input, queries, plugs; } fields[] = {
            {0x19a0, 0x12a, 0x84a}, {0x19d0, 0x21a, 0xb2e},
            {0x19f0, 0x256, 0xbde}, {0x1a10, 0x3be, 0x104a},
        };
        for (unsigned i = 0; i < sizeof fields / sizeof *fields; ++i) {
            unsigned p = fields[i].input;
            put32(data + p + 4, 0xffff); // source node -1, output index 0
            put32(data + p + 8, 0x800000); // Uses Default
            put32(data + p + 12, (uint32_t)slot);
            data[fields[i].queries] = data[fields[i].queries + 1] = 0;
            data[fields[i].plugs] = 0; // no longer a query-linked Int input
        }
    }
    return 1;
}
