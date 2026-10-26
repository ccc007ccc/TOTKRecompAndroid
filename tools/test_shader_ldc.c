// Execute production-emitted LDC code against a pre-instruction register/memory oracle.
// Outputs are written only to the directory supplied by the caller.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

int main(int argc, char **argv) {
    assert(argc == 2);
    char path[1024];
    snprintf(path, sizeof path, "%s/ldc.cpp", argv[1]);
    FILE *f = fopen(path, "w"); assert(f);
    fputs("#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
          "using uint=uint32_t; struct Buffer { uint data[16384]; uint *operator[](uint i){return data+4*i;} } cb5;\n"
          "uint read(Buffer &cb,uint off){return off<65536?cb.data[off/4]:0;}\n"
          "#define cbr(cb,off) read(cb,uint(off))\n", f);
    int count = 0;
    const int sources[] = {0, 1, 2, 3, RZ};
    const int dests[] = {0, 2};
    const int offsets[] = {0, 8, 0x1234, 0x7ff8, -32768, -32, -16, -8};
    for (int size = 4; size <= 5; size++)
    for (unsigned di = 0; di < sizeof dests / sizeof *dests; di++)
    for (unsigned ai = 0; ai < sizeof sources / sizeof *sources; ai++)
    for (unsigned oi = 0; oi < sizeof offsets / sizeof *offsets; oi++) {
        int d = dests[di], a = sources[ai], off = offsets[oi];
        uint64_t w = 0xef90000000000000ull | (uint64_t)size << 48 | 5ull << 36 |
                     (uint64_t)(uint16_t)off << 20 | 7ull << 16 | (uint64_t)a << 8 | d;
        ShaderInfo info = {0}; ShaderKey key = {.glsl = 1};
        Ctx c = {.info = &info, .key = &key, .glsl = true, .stage = STAGE_VERTEX};
        emit_insn(&c, w, 8); assert(!c.fail);
        fprintf(f, "void test%d(uint *r) { uint r0=r[0],r1=r[1],r2=r[2],r3=r[3];\n", count);
        fputs(c.body.buf, f);
        fputs("r[0]=r0;r[1]=r1;r[2]=r2;r[3]=r3;}\n", f);
        free(c.body.buf);

        // Compile the full generated shader, including static addressing and surrounding control flow.
        uint8_t blob[0xc0] = {0};
        uint64_t code[] = {0, w, 0xe30000000007000full, 0xe2400fffff87000full};
        memcpy(blob + 0x80, code, sizeof code);
        ShaderInfo *s = maxwell_translate(blob, 0x80 + sizeof code, STAGE_VERTEX, &key);
        assert(s->msl);
        snprintf(path, sizeof path, "%s/ldc-%d.vert", argv[1], count);
        FILE *glsl = fopen(path, "w"); assert(glsl); fputs(s->msl, glsl); fclose(glsl);
        maxwell_free(s);
        count++;
    }
    fputs("using Fn=void(*)(uint*); struct Case{Fn run;int a,d,off,n;}; Case cases[]={\n", f);
    count = 0;
    for (int size = 4; size <= 5; size++)
    for (unsigned di = 0; di < sizeof dests / sizeof *dests; di++)
    for (unsigned ai = 0; ai < sizeof sources / sizeof *sources; ai++)
    for (unsigned oi = 0; oi < sizeof offsets / sizeof *offsets; oi++)
        fprintf(f, "{test%d,%d,%d,%d,%d},\n", count++, sources[ai], dests[di], offsets[oi], size == 5 ? 2 : 1);
    fputs("}; uint state=0x51f123ab; uint next(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}\n"
          "int main(){for(uint &v:cb5.data)v=next();int total=0;\n"
          "for(auto c:cases)for(int i=0;i<1024;i++){uint r[4]={next(),next(),next(),next()},want[4];\n"
          "const uint boundary[]={0,4,8,16,32,32768,65520,65528,65532,65536,0xfffffff0,0xfffffffc};\n"
          "if(c.a!=255)r[c.a]=i<12?boundary[i]:(next()%16384)*4; std::memcpy(want,r,sizeof r);\n"
          "uint addr=(c.a==255?0:r[c.a])+c.off;for(int j=0;j<c.n;j++)want[c.d+j]=read(cb5,addr+4*j);\n"
          "c.run(r);if(std::memcmp(r,want,sizeof r)){std::printf(\"LDC a%d d%d off%x n%d failed\\n\",c.a,c.d,c.off,c.n);return 1;}total++;}\n"
          "std::printf(\"%d LDC register/memory vectors passed\\n\",total);}\n", f);
    fclose(f);
    printf("Generated %d LDC execution cases and vertex shaders\n", count);
    return 0;
}
