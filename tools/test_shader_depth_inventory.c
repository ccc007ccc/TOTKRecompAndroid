// Output-register packing must be measured from the real translator. Include
// sparse MRT masks and a predicated depth write: neither may be called identity.
#include <assert.h>
#include "../runtime/nvn/maxwell/shader.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const uint32_t masks[] = {0, 15, 0x1080, 0xffffffff};
    const unsigned depth_register[] = {1, 5, 3, 33};
    for (unsigned m = 0; m < 4; ++m)
    for (unsigned mode = 0; mode < 4; ++mode) {
        unsigned char blob[0xb0] = {0};
        uint32_t depth = mode != 3 ? 2 : 0;
        memcpy(blob + 0x7c, &depth, 4); memcpy(blob + 0x78, &masks[m], 4);
        unsigned reg = mode == 0 ? 200 : depth_register[m];
        uint64_t mov = 0x0100000000070000ull | (uint64_t)0x3f000000 << 20 | reg;
        if (mode == 2) mov &= ~(7ull << 16); // predicate p0 is initially false
        uint64_t code[] = {0, mov, mov, 0xe30000000007000full, 0, 0xe2400fffff87000full};
        memcpy(blob + 0x80, code, sizeof code);
        ShaderKey key = {.glsl = 1, .vs_out_mask = ~0u};
        ShaderInfo *s = maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &key);
        assert(s && s->msl && s->writes_depth == (mode != 3));
        assert(!!strstr(s->msl, "gl_FragDepth = gl_FragCoord.z;") == (mode == 0));
        if (mode == 1 || mode == 2) {
            char expected[80]; snprintf(expected, sizeof expected, "gl_FragDepth = as_f(r%u);", reg);
            assert(strstr(s->msl, expected));
        }
        maxwell_free(s);
    }
    puts("PASS: identity-depth inventory distinguishes sparse MRT packing, real/predicated depth writes and no depth");
}
