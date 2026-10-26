// Validate the production gather emitter across dimensions, shadow comparisons and offsets.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned count = 0;
    const int types[] = {TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE, TEXTYPE_CUBE_ARRAY};
    const char *names[] = {"2D", "2DArray", "Cube", "CubeArray"};
    for (int ti = 0; ti < 4; ti++) for (int shadow = 0; shadow < 2; shadow++)
    for (int enabled = 0; enabled < 2; enabled++) for (int offset = 0; offset < 2; offset++) {
        ShaderInfo info = {0}; ShaderKey key = {.glsl = 1, .gather_bias_bits = enabled ? 8 : 0};
        Ctx c = {.info = &info, .key = &key, .glsl = true, .stage = STAGE_FRAGMENT};
        info.tex[0].type = types[ti]; info.tex[0].shadow = shadow;
        const char *co[] = {"uv.x", "uv.y", "uv.z"}; char call[2000];
        glsl_gather(&c, call, 0, co, ti & 1 ? "layer" : NULL, "reference",
                    offset ? "ivec2(1, -1)" : NULL, shadow ? 0 : 2);
        assert(!c.fail);
        bool biased = enabled && ti < 2;
        assert((strstr(call, "0.001953125f") != NULL) == biased);
        assert((strstr(call, "textureSize(t0, 0).xy") != NULL) == biased);
        if (ti & 1) assert(strstr(call, "float(layer)"));
        if (shadow) assert(strstr(call, ", reference)"));
        char path[1024]; snprintf(path, sizeof path, "%s/gather-%u.frag", argv[1], count++);
        FILE *f = fopen(path, "w"); assert(f);
        fprintf(f, "#version 450\nlayout(set=0,binding=0) uniform sampler%s%s t0;\n"
                   "layout(location=0) in vec3 uv; layout(location=0) out vec4 color;\n"
                   "const uint layer=2u; const float reference=0.7; const vec4 rscale[2]=vec4[2](vec4(1.0),vec4(1.0));\n",
                names[ti], shadow ? "Shadow" : "");
        if (ti == 1 || ti == 3) fputs("ivec3 tsize0(int l){return textureSize(t0,l);}\n", f);
        else fputs("ivec3 tsize0(int l){return ivec3(textureSize(t0,l),1);}\n", f);
        fprintf(f, "void main(){color=%s;}\n", call); fclose(f);
    }
    printf("Generated %u gather cases; coordinate/layer/reference checks passed\n", count);
    return 0;
}
