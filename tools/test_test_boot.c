// Uses the player's original Sequence files, passed on the command line.
#include "../runtime/test_boot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    const char *names[] = {"Sequence/NormalTitle.140.module.ainb", "Sequence/OnLoadGameSaveData.module.ainb"};
    for (int k = 0; k < 2; ++k) {
        FILE *f = fopen(argv[k+1], "rb");assert(f);
        assert(!fseek(f, 0, SEEK_END));long size = ftell(f);assert(size > 0 && size < 32768);rewind(f);
        uint8_t *original = malloc(size), *data = malloc(size);assert(original && data);
        assert(fread(original, 1, size, f) == (size_t)size);fclose(f);
        for (int slot = 0; slot <= 5; ++slot) {
            memcpy(data, original, size);assert(test_boot_patch(names[k], data, size, slot) == 1);
            // Patched assets must not be accepted as fresh originals on a subsequent boot.
            uint8_t *again = malloc(size);assert(again);memcpy(again, data, size);
            assert(test_boot_patch(names[k], again, size, slot) == -1);
            assert(!memcmp(data, again, size));free(again);
        }
        for (int bad = -1; bad <= 6; bad += 7) {
            memcpy(data, original, size);assert(test_boot_patch(names[k], data, size, bad) == -1);
            assert(!memcmp(data, original, size));
        }
        memcpy(data, original, size);data[size-1] ^= 1;
        assert(test_boot_patch(names[k], data, size, 0) == -1);data[size-1] ^= 1;
        assert(!memcmp(data, original, size));
        assert(test_boot_patch(names[k], data, size-1, 0) == -1);
        assert(test_boot_patch("Sequence/Main.module.ainb", data, size, 0) == 0);
        assert(!memcmp(data, original, size));free(data);free(original);
    }
    puts("PASS: original sequence identities, all six slots, invalid slots, corrupt/truncated/repeated patches and unrelated files");
}
