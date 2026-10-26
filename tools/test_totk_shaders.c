// Public NVN control layout and synthetic Maxwell programs; no proprietary data.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/shader_control.h"
#include "../runtime/nvn/maxwell/shader.h"
static void put(uint8_t *p, size_t o, uint32_t v) { memcpy(p + o, &v, 4); }
int main(int argc, char **argv) {
    assert(argc == 2);
    uint8_t control[0x880] = {0}, blob[0x140] = {0};
    put(control, 0, 0x98761234); put(control, 0x20, sizeof control);
    put(control, 0x708, 0x100); put(control, 0x714, 5);
    put(control, 0x700, 0x120); put(control, 0x704, sizeof blob);
    NvnShaderControl info;
    assert(nvn_shader_control(control, sizeof control, &info));
    assert(info.entry == 0x100 && info.stage == 5 && info.constants == 0x120);
    assert(!nvn_shader_control(control, 0x87f, &info));
    put(control, 0x708, 0); assert(!nvn_shader_control(control, sizeof control, &info));
    put(control, 0x708, 0x30); put(control, 0x714, 1);
    assert(nvn_shader_control(control, sizeof control, &info));
    put(control, 0x714, 6); assert(!nvn_shader_control(control, sizeof control, &info));
    put(control, 0x714, 5); put(control, 0x708, 0x100);
    put(control, 0x700, 0x160); assert(!nvn_shader_control(control, sizeof control, &info));
    put(control, 0x700, 0x120);
    assert(nvn_shader_control(control, sizeof control, &info));
    // Deliberately invalid words in the compute header must never reach the instruction decoder.
    memset(blob, 0xff, 0x100);
    uint64_t code[] = {0, 0xe30000000007000full, 0xe2400fffff87000full, 0};
    memcpy(blob + info.entry, code, sizeof code);
    ShaderKey key = {.glsl = 1};
    ShaderInfo *shader = maxwell_translate_bounded(blob + info.entry, info.constants - info.entry,
                                                  info.size - info.constants, STAGE_COMPUTE, &key);
    assert(shader->msl); maxwell_free(shader);
    const uint64_t words[] = {
        0xebf9000001070806ull, 0xebf9000000870806ull, 0xebf9000000900007ull,
        0x50d8e3800007000eull, 0x3b34006060370102ull, 0x3a2c04e060b70c0bull, 0x18ff7f8400071721ull
    };
    maxwell_gmem_base = 0x340000000ull; maxwell_gmem_size = 0xd0000000ull;
    maxwell_gmem_map_all = true; maxwell_gmem_miss_log = true;
    unsigned count = 0;
    for (int mode = 0; mode < 2; mode++) for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
        for (int st = 0; st < 2; st++) {
            int stage = st ? STAGE_COMPUTE : STAGE_FRAGMENT;
            size_t start = st ? 0 : 0x80;
            uint8_t test[0xc0] = {0}; put(test, 0x78, 0xf);
            uint64_t instructions[] = {0, words[i], 0xe30000000007000full, 0xe2400fffff87000full};
            memcpy(test + start, instructions, sizeof instructions);
            maxwell_no_int64 = mode;
            shader = maxwell_translate(test, start + sizeof instructions, stage, &key);
            if (!shader->msl) { fprintf(stderr, "%s\n", shader->error); return 1; }
            char path[1024]; snprintf(path, sizeof path, "%s/totk-%d-%u.%s", argv[1], mode, i, st ? "comp" : "frag");
            FILE *f = fopen(path, "w"); assert(f); fputs(shader->msl, f); fclose(f);
            if (i <= 2) assert(shader->uses_gmem_store && shader->uses_gmem_map);
            maxwell_free(shader); count++;
        }
    }
    printf("NVN control/entry checks passed; generated %u shader validation cases\n", count);
}
