"""Exercise the production program lookup/reset with synthetic addresses, without a GPU."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / ".work/shader-program-cache-tests").resolve()
out.mkdir(parents=True, exist_ok=True)
source = (ROOT / "runtime/nvn/vulkan/exec.c").read_text()
program = source[source.index("typedef struct {\n    ShaderInfo *vi"):source.index("} Prog;") + len("} Prog;")]
cache = source[source.index("#define PROG_TAB_INITIAL"):source.index("// ================================================================== vertex formats")]
hash_function = source[source.index("static uint64_t hash_words(uint64_t h, const void *p, size_t n) {"):
                       source.index("static uint64_t hmix(uint64_t h, const void *p, size_t n) {")]
reset = source[source.index("void vk_exec_reset(void)"):source.index("static void texture_info(")]
assert "shader_programs_destroy();" in reset
assert "shader_object_find(&pipe_tab, ph)" in source and "shader_object_put(&pipe_tab, ph, pp)" in source
preamble = r'''
#include "runtime/nvn/vulkan/shaders.h"
#include "runtime/nvn/vulkan/shader_object_cache.h"
#include "runtime/nvn/nvn.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "runtime/nvn/vulkan/shader_code_state.h"
static ShaderCodeState shader_code_state;
#include "runtime/nvn/vulkan/shader_module_cache.h"
#define MAX_BIND 96
void rt_fatal_trace(void) { abort(); }
'''
stubs = r'''
static Prog *allocated[6008];
static unsigned translated;
static void translate_program(void *arg) {
    assert(translated < sizeof allocated / sizeof *allocated);
    Prog *p = arg;
    allocated[translated++] = p;
    // Translation fills the linked output mask after lookup has captured keys.
    p->fkey.vs_out_mask = 7;
    atomic_store(&p->state, 1);
}
static void run_translate(void (*fn)(void *), void *p) { fn(p); }
'''
test = r'''
int main(void) {
    NvnProgram np = {0}; np.stage_mask = 3;
    np.stages[0].code_addr = 0x1000; np.stages[1].code_addr = 0x2000;
    ShaderKey v = {0}, f = {0}; v.glsl = f.glsl = 1;
    Prog *first = get_program(&np, &v, &f);
    assert(first && get_program(&np, &v, &f) == first && translated == 1);
    ++np.generation; // SetShaders rebinds may reuse exactly the same addresses.
    Prog *rebound=get_program(&np,&v,&f);assert(rebound!=first && translated==2);
    shader_code_written(&shader_code_state,0x10000000,16);
    assert(get_program(&np,&v,&f)==rebound && translated==2);
    shader_code_written(&shader_code_state,0x1000,4);
    assert(get_program(&np,&v,&f)!=rebound && translated==3);
    program_cache_reset();
    Prog *reboot = get_program(&np, &v, &f);
    assert(reboot && reboot != first && translated == 4 && prog_tab.count == 1);
    Prog *programs[5000];
    unsigned before = translated;
    for (unsigned i = 0; i < 5000; ++i) {
        np.stages[0].code_addr = 0x10000ull + i * 0x1000ull;
        programs[i] = get_program(&np, &v, &f);
        assert(programs[i]->code[0] == np.stages[0].code_addr);
    }
    assert(translated == before + 5000 && prog_tab.count == 5001 && prog_tab.capacity > PROG_TAB_INITIAL);
    for (unsigned i = 0; i < 5000; ++i) {
        np.stages[0].code_addr = 0x10000ull + i * 0x1000ull;
        assert(get_program(&np, &v, &f) == programs[i]);
    }
    assert(translated == before + 5000);
    v.attr_kind[0] = 2;
    assert(get_program(&np, &v, &f) != programs[4999]);
    before = translated;
    program_cache_reset();
    assert(prog_tab.count == 0 && !prog_last.prog);
    assert(get_program(&np, &v, &f) && translated == before + 1);
    program_cache_reset();
    for (unsigned i = 0; i < translated; ++i) free(allocated[i]);
    free(prog_tab.entries);
    shader_code_reset(&shader_code_state);
    ShaderObjectTable pipelines = {.initial_capacity = 8192};
    int objects[10000];
    for (unsigned i = 0; i < 10000; ++i) {
        // Deliberately collide in the original low buckets to exercise probing.
        uint64_t key = 1 + ((uint64_t)i << 32);
        assert(shader_object_put(&pipelines, key, &objects[i]));
    }
    assert(pipelines.count == 10000 && pipelines.capacity > 8192);
    for (unsigned i = 0; i < 10000; ++i)
        assert(shader_object_find(&pipelines, 1 + ((uint64_t)i << 32)) == &objects[i]);
    assert(shader_object_put(&pipelines, 1, &objects[1]) && pipelines.count == 10000);
    assert(shader_object_find(&pipelines, 1) == &objects[1]);
    assert(!shader_object_put(&pipelines, 0, &objects[0]) && !shader_object_find(&pipelines, 2));
    shader_object_clear(&pipelines);
    assert(!pipelines.count && !shader_object_find(&pipelines, 1));
    free(pipelines.entries);
    ShaderObjectTable invalid = {.initial_capacity = 3};
    assert(!shader_object_put(&invalid, 1, &objects[0]) && !invalid.entries);
    puts("PASS: code rebind/write invalidation; reboot clears last-program shortcut; 5000 programs and 10000 pipeline entries grow, reuse and reset");
}
'''
(out / "programs.c").write_text(preamble + program + hash_function + stubs + cache + test)
subprocess.run(["clang", "-O1", "-g", "-fsanitize=address,undefined", "-I", str(ROOT),
                "-I", str(ROOT / "android/deps/out/include"), "-I", str(ROOT / "android/deps/out/volk"),
                str(out / "programs.c"), "-o", str(out / "test-programs")], check=True)
subprocess.run([str(out / "test-programs")], check=True, timeout=15)

# Compute uses its own cache: local workgroup metadata must follow rebinding too.
cs_type = source[source.index("typedef struct { Prog p; ShaderInfo *si;"):source.index("\n\nstatic void build_compute")]
cs_table = source[source.index("static struct CsEnt"):source.index("static void shader_programs_destroy(void) {")]
start = source.index("static CsProg *compute_program(")
cs_function = source[start:source.index("\n}\n", start)+3]
cs_stubs = r"""
static int g_cs_n;
static unsigned computed;
void vk_shader_prepare_configure(void) {}
static uint16_t compute_image_kinds(void) {return 0;}
static unsigned gather_bias_bits(void) {return 8;}
static void vk_shader_compute_key(ShaderKey* key,uint16_t kinds){(void)kinds;memset(key,0,sizeof *key);key->glsl=1;key->gather_bias_bits=8;}
static pthread_mutex_t translate_lock=PTHREAD_MUTEX_INITIALIZER;
ShaderInfo* maxwell_translate_bounded(const uint8_t* code,size_t bytes,size_t constants,int stage,const ShaderKey* key) {
    (void)code;(void)bytes;(void)constants;(void)key;assert(stage==STAGE_COMPUTE);
    ShaderInfo* info=calloc(1,sizeof *info);info->msl=strdup("test");return info;
}
static void save_glsl(const char* text,uint64_t addr,const char* suffix){(void)text;(void)addr;(void)suffix;}
static ShaderInfo* load_or_translate_stage(const NvnShaderStage* input,const uint8_t* code,size_t bytes,size_t constants,
                                          int stage,const ShaderKey* key,Prog* p,unsigned slot) {
    (void)input;(void)p;(void)slot;return maxwell_translate_bounded(code,bytes,constants,stage,key);
}
static void build_compute(void* p){++computed;atomic_store(&((CsProg*)p)->p.state,1);}
static void run_job(void(*fn)(void*),void* p){fn(p);}
"""
cs_test = r"""
int main(void) {
    uint8_t control[0x880]={0};uint32_t size=4;memcpy(control+0x748,&size,4);
    NvnProgram np={0};np.generation=1;
    np.stages[5]=(NvnShaderStage){.code_addr=0x100000,.control=control,.entry_offset=0x100,.const_offset=0x200,.const_size=32};
    CsProg* first=compute_program(&np);assert(first->local[0]==4 && computed==1);
    assert(compute_program(&np)==first && computed==1);
    size=8;memcpy(control+0x748,&size,4);++np.generation;
    CsProg* second=compute_program(&np);assert(second!=first && second->local[0]==8 && computed==2);
    shader_code_written(&shader_code_state,0x100000,4);
    assert(compute_program(&np)!=second && computed==3);
    for(int i=0;i<g_cs_n;++i){free(g_cs_tab[i].cp->si->msl);free(g_cs_tab[i].cp->si);free(g_cs_tab[i].cp);}
    free(g_cs_tab);shader_code_reset(&shader_code_state);
    puts("PASS: compute same-address rebind refreshes workgroup metadata; code writes invalidate compute lookup");
}
"""
(out / "compute.c").write_text(preamble.replace('#define MAX_BIND', '#include "runtime/nvn/vulkan/pipeline_warm.h"\n#define MAX_BIND') +
                                program + cs_type + cs_table + cs_stubs + cs_function + cs_test)
subprocess.run(["clang", "-O1", "-g", "-fsanitize=address,undefined", "-pthread", "-I", str(ROOT),
                "-I", str(ROOT / "android/deps/out/include"), "-I", str(ROOT / "android/deps/out/volk"),
                str(out / "compute.c"), "-o", str(out / "test-compute")], check=True)
subprocess.run([str(out / "test-compute")], check=True, timeout=15)
