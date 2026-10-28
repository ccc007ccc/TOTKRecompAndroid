// Exercise the production sampling emitter, including the stage/type boundary of the Maxwell subtexel correction.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"
#include "../runtime/nvn/vulkan/shader_cache.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    const int types[] = {TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_3D, TEXTYPE_CUBE};
    const char *names[] = {"2D", "2DArray", "3D", "Cube"};
    unsigned count = 0;
    {
        ShaderInfo info={0};ShaderKey normal={.glsl=1},experiment={.glsl=1,.vertex_snap=true};
        Ctx c={.info=&info,.key=&normal,.glsl=true,.stage=STAGE_VERTEX};info.tex[0].type=TEXTYPE_2D;
        const char *co[]={"uv.x","uv.y"};char native[4096],snapped[4096];
        glsl_sample(&c,native,0,co,NULL,NULL,1,"0.0",NULL);
        c.key=&experiment;glsl_sample(&c,snapped,0,co,NULL,NULL,1,"0.0",NULL);
        assert(!strstr(native,"256.0f")&&strstr(snapped,"256.0f"));
        assert(memcmp(&normal,&experiment,sizeof normal));
        assert(vk_shader_source_hash(native,STAGE_VERTEX,false)!=vk_shader_source_hash(snapped,STAGE_VERTEX,false));
    }
    for (unsigned type = 0; type < 4; type++) for (unsigned fragment = 0; fragment < 2; fragment++)
    for (unsigned offset = 0; offset < 2; offset++) {
        ShaderInfo info = {0}; ShaderKey key = {.glsl = 1,.vertex_snap=true};
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
    // Explicit texture LODs may be negative or beyond the view's mip range. Exercise both without invalid size queries.
    for(unsigned i=0;i<2;i++) {
        ShaderInfo info={0};ShaderKey key={.glsl=1,.vertex_snap=true};
        Ctx c={.info=&info,.key=&key,.glsl=true,.stage=STAGE_VERTEX};info.tex[0].type=TEXTYPE_2D;
        const char *co[]={"uv.x","uv.y"};char call[4096],path[1024];
        glsl_sample(&c,call,0,co,NULL,NULL,1,i?"99.0":"-1.0",NULL);assert(!c.fail);
        snprintf(path,sizeof path,"%s/snap-bound-%u.vert",argv[1],i);FILE*f=fopen(path,"w");assert(f);
        fprintf(f,"#version 450\nlayout(set=0,binding=0) uniform sampler2D t0;\n"
                  "layout(location=0) in vec3 uv;layout(location=0) out vec4 color;\n"
                  "ivec3 tsize0(int l){return ivec3(textureSize(t0,l),1);}\n"
                  "void main(){color=%s;gl_Position=vec4(uv,1);}\n",call);fclose(f);count++;
    }
    printf("Generated %u vertex/fragment sampling cases\n", count);
}
