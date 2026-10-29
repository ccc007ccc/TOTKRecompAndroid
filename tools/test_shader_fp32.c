// Translator/SPIR-V regression generator; conversions.cpp runs on the host CPU.
// No GPU execution or binary64 arithmetic accuracy claim is made by this test.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

static unsigned count;
static void shader(const char *dir, uint64_t word, bool native, const char *kind) {
    uint8_t data[0xa0] = {0};
    uint64_t code[] = {0, word, 0xe30000000007000full, 0xe2400fffff87000full};
    memcpy(data + 0x80, code, sizeof code);
    ShaderKey key = {.glsl = 1, .vs_out_mask = ~0u};
    ShaderInfo *s = maxwell_translate(data, sizeof data, STAGE_VERTEX, &key);
    if (!s->msl) { fprintf(stderr, "%016llx: %s\n", (unsigned long long)word, s->error); abort(); }
    assert((strstr(s->msl, "precise float fr_") != NULL) == native);
    if (native) assert(!strstr(s->msl, "uvec2 dr_ = sf64_add") &&
                       !strstr(s->msl, "uvec2 dr_ = sf64_mul") && !strstr(s->msl, "uvec2 dr_ = sf64_fma"));
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%u.vert", dir, native ? kind : "soft", count++);
    FILE *f = fopen(path, "w"); assert(f); fputs(s->msl, f); assert(!fclose(f));
    maxwell_free(s);
}

static void conversion_cpu_test(const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/conversions.cpp", dir);
    FILE *f = fopen(path, "w"); assert(f);
    fputs("#include <cassert>\n#include <cfenv>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
          "#pragma STDC FENV_ACCESS ON\nusing uint=uint32_t;\n"
          "struct uvec2 {uint x,y;uvec2(uint a,uint b):x(a),y(b){}};\n"
          "uvec2 sf64_from_small(uint,int,int){assert(false);return uvec2(0,0);}\n"
          "uint sf64_to_small(uvec2,int,int,int,uint){assert(false);return 0;}\n", f);
    // Execute the actual emitted normal-range conversion code against the CPU's
    // IEEE conversions in all rounding modes. Out-of-range cases use the retained
    // generic helper and are outside this fast-path regression.
    const char *names[] = {"uvec2 sf64_from_f32(", "uint sf64_to_f32("};
    for (unsigned i=0; i<2; ++i) {
        const char *start = strstr(glsl_soft64_helpers, names[i]); assert(start);
        const char *end = strstr(start, "\n}\n"); assert(end);
        fwrite(start, 1, end + 3 - start, f);
    }
    fputs("uint state=0x59624328u;uint next(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}\n"
          "int main(){unsigned long long tests=0;\n"
          "for(uint e=1;e<255;e++)for(uint sign=0;sign<2;sign++)for(uint k=0;k<1024;k++){\n"
          "uint raw=(sign<<31)|(e<<23)|(next()&0x7fffff);float a;std::memcpy(&a,&raw,4);\n"
          "double expected=a;uint64_t bits;std::memcpy(&bits,&expected,8);uvec2 z=sf64_from_f32(raw);\n"
          "assert((uint64_t(z.y)<<32|z.x)==bits);tests++;}\n"
          "int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};\n"
          "uint edges[]={0u,1u,0xfffffff,0x10000000u,0x10000001u,0x1ffffffeu,0x1fffffffu,0xffffffffu};\n"
          "for(uint mode=0;mode<4;mode++){assert(!std::fesetround(modes[mode]));\n"
          "for(uint e=897;e<=1150;e++)for(uint sign=0;sign<2;sign++)for(uint k=0;k<1024;k++){\n"
          "uint lo=k<64?edges[k%8]:next();uint frac=k<8?0xfffffu:k<16?0u:next()&0xfffffu;\n"
          "uint hi=(sign<<31)|(e<<20)|frac;uint64_t raw=uint64_t(hi)<<32|lo;double a;std::memcpy(&a,&raw,8);\n"
          "volatile double input=a;volatile float result=static_cast<float>(input);float expected=result;\n"
          "uint bits;std::memcpy(&bits,&expected,4);assert(sf64_to_f32(uvec2(lo,hi),mode)==bits);tests++;}}\n"
          "assert(!std::fesetround(FE_TONEAREST));std::printf(\"%llu normal-range conversion checks passed\\n\",tests);}\n", f);
    assert(!fclose(f));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    maxwell_no_int64 = true;
    maxwell_gmem_base = 0x340000000ull; maxwell_gmem_size = 0xd0000000ull;
    const uint64_t ops[] = {
        0x5c70000000000000ull, 0x4c70000000000000ull, 0x3870000000000000ull,
        0x5c80000000000000ull, 0x4c80000000000000ull, 0x3880000000000000ull,
        0x5b70000000000000ull, 0x4b70000000000000ull, 0x3670000000000000ull, 0x5370000000000000ull,
    };
    for (unsigned enabled=0; enabled<2; ++enabled) {
        maxwell_fp32_double = enabled;
        for (unsigned op=0; op<10; ++op) for (unsigned mode=0; mode<4; ++mode) {
            uint64_t w = ops[op] | 7ull<<16 | 4ull<<8 | 6ull<<20;
            w |= (uint64_t)mode << (op>=6 ? 50 : 39);
            if (op>=6) w |= 8ull<<39;
            if (op==1 || op==4 || op==7 || op==9) w |= 3ull<<34;
            shader(argv[1], w, enabled && mode==0, op<3 ? "add" : op<6 ? "mul" : "fma");
        }
        // Pair aliases, RZ reads/writes, predicate guard and operand modifiers.
        shader(argv[1], ops[0]|4ull|4ull<<8|6ull<<20|7ull<<16|1ull<<46|1ull<<48|1ull<<49|1ull<<45, enabled, "add");
        shader(argv[1], ops[3]|254ull|255ull<<8|255ull<<20|7ull<<16|1ull<<48, enabled, "mul");
        shader(argv[1], ops[6]|255ull|4ull<<8|6ull<<20|8ull<<39|1ull<<16|1ull<<49, enabled, "fma");
        // F32->F64, F64->F32 and F64->F64 (integral rounding).
        shader(argv[1], 0x5ca8000000000000ull|3ull<<8|2ull<<10|4ull<<20|7ull<<16, false, "soft");
        shader(argv[1], 0x5ca8000000000000ull|2ull<<8|3ull<<10|4ull<<20|7ull<<16, false, "soft");
        shader(argv[1], 0x5ca8000000000000ull|3ull<<8|3ull<<10|4ull<<20|7ull<<16|1ull<<42, false, "soft");
        uint64_t invalid[] = {ops[0]|1ull, ops[3]|3ull<<8, ops[6]|1ull<<47};
        for (unsigned i=0; i<3; ++i) {
            ShaderInfo info={0}; ShaderKey key={.glsl=1};
            Ctx c={.info=&info,.key=&key,.glsl=true};
            assert(emit_fp64(&c, invalid[i])); assert(c.fail); free(c.body.buf);
        }
    }
    // A shader without doubles must keep the same cache source across modes.
    uint8_t data[0xa0]={0};
    uint64_t code[]={0,0xe30000000007000full,0xe30000000007000full,0xe2400fffff87000full};
    memcpy(data+0x80,code,sizeof code);
    ShaderKey key={.glsl=1}; maxwell_fp32_double=false;
    ShaderInfo *a=maxwell_translate(data,sizeof data,STAGE_VERTEX,&key);
    maxwell_fp32_double=true; ShaderInfo *b=maxwell_translate(data,sizeof data,STAGE_VERTEX,&key);
    assert(a->msl && b->msl && !strcmp(a->msl,b->msl)); maxwell_free(a);maxwell_free(b);
    conversion_cpu_test(argv[1]);
    printf("Generated %u shaders; native/fallback selection, pair boundaries and unchanged-source checks passed\n",count);
}
