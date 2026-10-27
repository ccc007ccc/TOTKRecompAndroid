// Exercise the production sampling emitter, including the stage/type boundary of the Maxwell subtexel correction.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

int main(int argc, char **argv) {
    assert(argc == 2);
    const int types[] = {TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_3D, TEXTYPE_CUBE};
    const char *names[] = {"2D", "2DArray", "3D", "Cube"};
    unsigned count = 0;
    for (unsigned type = 0; type < 4; type++) for (unsigned fragment = 0; fragment < 2; fragment++)
    for (unsigned offset = 0; offset < 2; offset++) {
        ShaderInfo info = {0}; ShaderKey key = {.glsl = 1};
        Ctx c = {.info = &info, .key = &key, .glsl = true, .stage = fragment ? STAGE_FRAGMENT : STAGE_VERTEX};
        info.tex[0].type = types[type];
        const char *co[] = {"uv.x", "uv.y", "uv.z"}; char call[4096];
        glsl_sample(&c, call, 0, co, type == 1 ? "layer" : NULL, "0.7", 1, "0.0", offset ? "ivec2(1,-1)" : NULL);
        assert(!c.fail);
        assert((strstr(call, "256.0f") != NULL) == (!fragment && type < 2));
        if (type == 1) assert(strstr(call, "float(layer)"));
        char path[1024]; snprintf(path, sizeof path, "%s/snap-%u.%s", argv[1], count++, fragment ? "frag" : "vert");
        FILE *f = fopen(path, "w"); assert(f);
        fprintf(f, "#version 450\nlayout(set=0,binding=0) uniform sampler%s t0;\n"
                   "layout(location=0) in vec3 uv;layout(location=0) out vec4 color;\n"
                   "const uint layer=2u;const vec4 rscale[2]=vec4[2](vec4(1),vec4(1));\n", names[type]);
        if (type == 1 || type == 2) fputs("ivec3 tsize0(int l){return textureSize(t0,l);}\n", f);
        else fputs("ivec3 tsize0(int l){return ivec3(textureSize(t0,l),1);}\n", f);
        fprintf(f, "void main(){color=%s;%s}\n", call, fragment ? "" : "gl_Position=vec4(uv,1);");
        fclose(f);
    }
    printf("Generated %u vertex/fragment sampling cases\n", count);
}
