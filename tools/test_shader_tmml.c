// Execute the production TMML emitter against independent Maxwell component semantics.
// The same emitted fragments can be run on a GPU with a real mipmapped sampler.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

int main(int argc, char **argv) {
    assert(argc == 2);
    char path[1024];
    snprintf(path, sizeof path, "%s/tmml.cpp", argv[1]);
    FILE *test = fopen(path, "w"); assert(test);
    fputs("#include <cstdint>\n#include <cstdio>\n#include <cmath>\n"
          "using uint=uint32_t; struct vec2 {float x,y;vec2(float a,float b):x(a),y(b){}};\n"
          "struct uvec4 {uint v[4];uvec4(uint a,uint b,uint c,uint d):v{a,b,c,d}{} uint operator[](int i){return v[i];}};\n"
          "float as_f(uint){return 0;} int t0; vec2 queried(0,0);\n"
          "vec2 textureQueryLod(int,vec2){return queried;}\n", test);
    for (int bindless = 0; bindless < 2; bindless++) for (int mask = 1; mask <= 3; mask++) {
        ShaderInfo info = {0}; ShaderKey key = {.glsl = 1};
        // Bindless handle from c[5][0x20], with TMML reading register 2.
        uint64_t code[] = {0, 0x4c98000000000000ull | 5ull << 34 | 8ull << 20 | 7ull << 16 | 2};
        Ctx c = {.info = &info, .key = &key, .glsl = true, .stage = STAGE_FRAGMENT,
                 .code = (const uint8_t *)code, .ninsts = 2};
        uint64_t w = (bindless ? 0xdf60000000000000ull : 0xdf58000000000000ull) |
                     (uint64_t)mask << 31 | 1ull << 29 | 2ull << 20 | 7ull << 16 | 8;
        if (!bindless) w |= 8ull << 36;
        emit_tmml(&c, w, bindless, 16);
        assert(!c.fail && info.ntex == 1);
        assert(info.tex[0].type == TEXTYPE_2D);
        assert(info.tex[0].bindless == !!bindless);
        if (bindless) assert(info.tex[0].bindless_cbuf == 5 && info.tex[0].bindless_offset == 0x20);
        Str body = {0}; glsl_rewrite(&c, &body, c.body.buf);
        int id = bindless * 3 + mask - 1;
        fprintf(test, "void test%d(uint& r8,uint& r9){uint r0=0,r1=0;\n%s}\n", id, body.buf);
        snprintf(path, sizeof path, "%s/tmml-%d.frag", argv[1], id);
        FILE *shader = fopen(path, "w"); assert(shader);
        fputs("#version 450\nlayout(set=0,binding=32) uniform sampler2D t0;\n"
              "layout(location=0) in vec2 uv;layout(location=0) out vec4 color;\n"
              "float as_f(uint x){return uintBitsToFloat(x);}\n"
              "void main(){uint r0=floatBitsToUint(uv.x),r1=floatBitsToUint(uv.y),r8=0u,r9=0u;\n", shader);
        fputs(body.buf, shader);
        // x/y carry signed results; z mirrors the cloud's unsigned I2F and /256.
        fprintf(shader, "color=vec4(float(int(r8))/256.0,float(int(r9))/256.0,float(%s)/256.0,1);}\n",
                mask == 3 ? "r9" : "r8");
        assert(fclose(shader) == 0);
        free(body.buf); free(c.body.buf);
    }
    fputs("using Fn=void(*)(uint&,uint&);Fn cases[]={test0,test1,test2,test3,test4,test5};\n"
          "int main(){const float values[][2]={{0,-3.25f},{0,-0.5f},{0,0},{2.75f,2.75f},{1.5f,5.125f},{4.0f,8.875f}};\n"
          "int checked=0;for(int i=0;i<6;i++)for(auto &v:values){queried=vec2(v[0],v[1]);\n"
          "uint a=0x12345678,b=0x87654321;cases[i](a,b);int mask=i%3+1;\n"
          "uint wantA=uint(int32_t(v[(mask&1)?1:0]*256.0f));\n"
          "uint wantB=mask==3?uint(int32_t(v[0]*256.0f)):0x87654321;\n"
          "if(a!=wantA||b!=wantB){printf(\"FAIL case %d: accessed=%g computed=%g, got %08x/%08x expected %08x/%08x\\n\",i,v[0],v[1],a,b,wantA,wantB);return 1;}checked++;}\n"
          "printf(\"PASS: %d TMML bound/bindless, result-mask, signed fractional and sampler-clamp vectors\\n\",checked);}\n", test);
    assert(fclose(test) == 0);
    puts("Generated TMML execution checks and six GPU fragments");
    return 0;
}
