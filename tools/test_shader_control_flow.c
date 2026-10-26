// Synthetic Maxwell control flow: execute the production GLSL body as C++ and
// validate complete shaders. No game data or GPU-specific expected values.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

typedef struct { uint64_t words[256]; unsigned n; } Program;
static unsigned next_pc(Program *p) { return (p->n + (p->n % 4 == 0)) * 8; }
static unsigned insn(Program *p, uint64_t w) {
    if (p->n % 4 == 0) p->n++;
    unsigned pc = p->n * 8;
    assert(p->n < 256); p->words[p->n++] = w;
    return pc;
}
static uint64_t mov(int d, uint32_t v) { return 0x0100000000070000ull | (uint64_t)v << 20 | d; }
static uint64_t add(int d, int a, uint32_t v) { return 0x1c00000000070000ull | (uint64_t)v << 20 | a << 8 | d; }
static uint64_t ldc(int d) { return 0xef9400000007ff00ull | d; }
static uint64_t cmp(int pred, int a, int b, int op) {
    return 0x5b60000000070000ull | (uint64_t)op << 49 | 7ull << 39 | (uint64_t)b << 20 | a << 8 | pred << 3 | 7;
}
static unsigned branch(Program *p, int pred) {
    return insn(p, 0xe24000000000000full | (uint64_t)pred << 16);
}
static void target(Program *p, unsigned at, unsigned to) {
    p->words[at / 8] |= ((uint64_t)(int64_t)((int)to - (int)at - 8) & 0xffffff) << 20;
}
static void finish(Program *p) {
    insn(p, 0xe30000000007000full);
    unsigned at = branch(p, PT); target(p, at, at);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    Program programs[4] = {0};
    Program *p = &programs[0];
    insn(p, ldc(0)); insn(p, mov(1, 10)); insn(p, mov(2, 0));
    insn(p, cmp(0, 0, 2, 2)); unsigned zero = branch(p, 0);
    insn(p, mov(1, 25)); unsigned join = branch(p, PT);
    target(p, zero, next_pc(p)); insn(p, mov(1, 40));
    target(p, join, next_pc(p)); insn(p, add(1, 1, 3));
    insn(p, mov(2, 2)); insn(p, cmp(1, 0, 2, 2)); unsigned bonus = branch(p, 1);
    insn(p, add(1, 1, 5)); unsigned done = branch(p, PT);
    target(p, bonus, next_pc(p)); insn(p, add(1, 1, 11));
    target(p, done, next_pc(p)); finish(p);

    // The untaken branch creates another label inside each loop iteration.
    // 60,000 iterations must still fit the original 100,000-branch guard.
    p = &programs[1]; insn(p, ldc(0)); insn(p, mov(1, 0));
    unsigned loop = next_pc(p); unsigned never = branch(p, 15); // @!PT
    insn(p, add(1, 1, 1)); target(p, never, next_pc(p));
    insn(p, cmp(0, 1, 0, 1)); unsigned back = branch(p, 0); target(p, back, loop);
    finish(p);

    // Both paths pop the same reconvergence target, with dynamic and resolved stacks.
    p = &programs[2]; insn(p, ldc(0)); insn(p, mov(2, 0));
    unsigned ssy = insn(p, 0xe290000000000000ull);
    insn(p, cmp(0, 0, 2, 2)); zero = branch(p, 0);
    insn(p, mov(1, 11)); insn(p, 0xf0f8000000070000ull);
    target(p, zero, next_pc(p)); insn(p, mov(1, 31)); insn(p, 0xf0f8000000070000ull);
    target(p, ssy, next_pc(p)); insn(p, add(1, 1, 7)); finish(p);

    // Computed dispatch, including a target on a scheduling word and the default exit.
    p = &programs[3]; insn(p, ldc(0));
    unsigned brx = insn(p, 0xe25000000007000full);
    insn(p, 0x50b0000000070000ull); // first case follows a scheduling word
    unsigned first = next_pc(p); insn(p, mov(1, 17)); join = branch(p, PT);
    unsigned second = next_pc(p); insn(p, mov(1, 29)); done = branch(p, PT);
    target(p, join, next_pc(p)); target(p, done, next_pc(p)); insn(p, add(1, 1, 5)); finish(p);

    char path[1024]; snprintf(path, sizeof path, "%s/flow.cpp", argv[1]);
    FILE *cpp = fopen(path, "w"); assert(cpp);
    fputs("#include <cstdint>\n#include <cstdio>\nusing uint=uint32_t;\nuint cb0[16][4];\n", cpp);
    int functions = 0, shaders = 0;
    for (int fixture = 0; fixture < 4; fixture++)
    for (int stacks = 0; stacks < 2; stacks++)
    for (int structured = 0; structured < 2; structured++)
    for (int explicit_next = 0; explicit_next < 2; explicit_next++) {
        maxwell_static_stacks = stacks;
        maxwell_structured = structured;
        maxwell_explicit_fallthrough = explicit_next;
        p = &programs[fixture];
        uint8_t blob[0x1000] = {0}; memcpy(blob + 0x80, p->words, p->n * 8);
        for (int stage_index = 0; stage_index < 3; stage_index++) {
            int stage = stage_index == 2 ? STAGE_COMPUTE : stage_index;
            ShaderKey key = {.glsl = 1};
            ShaderInfo *s = maxwell_translate(stage == STAGE_COMPUTE ? blob + 0x80 : blob,
                p->n * 8 + (stage == STAGE_COMPUTE ? 0 : 0x80), stage, &key);
            if (!s->msl) { fprintf(stderr, "%s\n", s->error); return 1; }
            snprintf(path, sizeof path, "%s/flow-%d-%d-%d-%d.%s", argv[1], fixture, stacks, structured, explicit_next,
                stage == STAGE_COMPUTE ? "comp" : stage == STAGE_FRAGMENT ? "frag" : "vert");
            FILE *f = fopen(path, "w"); assert(f); fputs(s->msl, f); fclose(f); shaders++;
            if (stage == STAGE_FRAGMENT) {
                const char *body = strstr(s->msl, "void main()") + strlen("void main()");
                const char *end = strrchr(body, '}'); assert(end);
                fprintf(cpp, "uint run%d()%.*sreturn r1;\n}\n", functions, (int)(end - body), body);
            }
            maxwell_free(s);
        }
        // The workaround is specific to GLSL, even for branch-heavy shaders.
        ShaderKey metal = {0}; maxwell_explicit_fallthrough = false;
        ShaderInfo *a = maxwell_translate(blob, 0x80 + p->n * 8, STAGE_VERTEX, &metal);
        maxwell_explicit_fallthrough = true;
        ShaderInfo *b = maxwell_translate(blob, 0x80 + p->n * 8, STAGE_VERTEX, &metal);
        assert(a->msl && b->msl && !strcmp(a->msl, b->msl)); maxwell_free(a); maxwell_free(b);
        functions++;
    }
    fputs("using Fn=uint(*)(); struct Case{Fn run;int fixture;}; Case cases[]={\n", cpp);
    for (int i = 0; i < functions; i++) fprintf(cpp, "{run%d,%d},\n", i, i / 8);
    fputs("};\nint main(){unsigned checks=0; for(auto c:cases){\n", cpp);
    fprintf(cpp, "const uint jumps[]={%u,%u,%u,0xffffff00u};\n", first - brx - 8, second - brx - 8, first - brx - 16);
    fputs("for(uint i=0;i<256;i++){uint input=i,expected=0;\n"
        "if(c.fixture==0)expected=input==0?48:input==2?39:33;\n"
        "if(c.fixture==1){input=i==255?60000:i;expected=input?input:1;}\n"
        "if(c.fixture==2)expected=input==0?38:18;\n"
        "if(c.fixture==3){input=jumps[i%4];expected=i%4==1?34:i%4==3?0:22;}\n"
        "cb0[0][0]=input;uint result=c.run();if(result!=expected){\n"
        "std::printf(\"fixture %d input %u: %u != %u\\n\",c.fixture,input,result,expected);return 1;}checks++;}}\n"
        "std::printf(\"%u control-flow execution checks passed\\n\",checks);}\n", cpp);
    fclose(cpp);
    printf("Generated %d control-flow shaders and %d execution functions; Metal output unchanged\n", shaders, functions);
    return 0;
}
