"""Execute actual graphics/compute translation handoff without losing first-use draws."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root/'runtime/nvn/vulkan/exec.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}\n', start)+3]
program = source[source.index('typedef struct {\n    ShaderInfo *vi'):source.index('} Prog;')+7]
helpers = source[source.index('static bool shader_stage_raw_key('):source.index('#include "shader_code_state.h"')]
preamble = r'''
#include "runtime/nvn/vulkan/shaders.h"
#include "runtime/nvn/vulkan/shader_artifact.h"
#include "runtime/nvn/vulkan/shader_cache.h"
#include "runtime/nvn/vulkan/shader_prepare.h"
#include "runtime/nvn/vulkan/shader_runtime.h"
#include "runtime/nvn/vulkan/pipeline_warm.h"
#include "runtime/nvn/vulkan/shader_code_state.h"
#include "runtime/nvn/vulkan/shader_module_cache.h"
#include "runtime/nvn/vulkan/shader_keys.h"
#include "runtime/diagnostics_build.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#define MAX_BIND 96
VkG g_vk;
void rt_fatal_trace(void){abort();}
static pthread_mutex_t translate_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned translated,compile_calls,saved_sources;
static int fail_stage=-1;
static _Atomic unsigned stat_compiles;
static int g_cs_n;
static ShaderCodeState shader_code_state;
void vk_shader_prepare_configure(void){maxwell_no_int64=true;maxwell_fp32_double=true;}
uint64_t vk_shader_prepare_profile(void){return 123;}
unsigned vk_shader_prepare_gather(void){return 8;}
bool vk_shader_prepare_vertex_snap(void){return false;}
bool vk_shader_prepare_compute_linear(void){return false;}
static ShaderInfo* counted_translate(const uint8_t* code,size_t bytes,size_t constants,int stage,const ShaderKey* key){
    ++translated;if(stage==fail_stage)return NULL;
    return maxwell_translate_bounded(code,bytes,constants,stage,key);
}
#define maxwell_translate_bounded counted_translate
'''
stubs = r'''
static uint32_t *reference[2];static size_t reference_words[2];static bool compare;
static bool graphics_program_modules(Prog* p,const uint32_t* v,size_t nv,const uint32_t* f,size_t nf){
    const uint32_t* modules[]={v,f};size_t words[]={nv,nf};
    for(unsigned i=0;i<2;++i){
        assert(modules[i] && vk_shader_cache_framed(modules[i],words[i]));
        if(compare)assert(words[i]==reference_words[i] && !memcmp(modules[i],reference[i],words[i]*4));
        else {free(reference[i]);reference[i]=malloc(words[i]*4);assert(reference[i]);memcpy(reference[i],modules[i],words[i]*4);reference_words[i]=words[i];}
    }
    p->nbinds=vk_reflect(v,nv,p->binds,MAX_BIND);
    p->nbinds+=vk_reflect(f,nf,p->binds+p->nbinds,MAX_BIND-p->nbinds);
    return true;
}
static void save_glsl(const char* text,uint64_t code,const char* suffix){(void)code;(void)suffix;assert(text);++saved_sources;}
static void vertex_debug_instrument(ShaderInfo* i,const ShaderKey* k,uint64_t c){(void)k;(void)c;assert(i->msl);}
static void run_job(void(*fn)(void*),void* arg){++compile_calls;fn(arg);}
static unsigned gather_bias_bits(void){return 8;}
static uint16_t compute_image_kinds(void){return 0;}
static VkPipelineLayout make_layout(Prog* p,int n,VkShaderStageFlags a,VkShaderStageFlags b){(void)p;(void)n;(void)a;(void)b;return VK_NULL_HANDLE;}
static uint32_t* compute_reference;static size_t compute_words;static bool compare_compute;
static bool mock_module_acquire(uint64_t key,int stage,const uint32_t* words,size_t n,VkShaderModuleLease* out){
    (void)key;(void)stage;(void)out;
    assert(vk_shader_cache_framed(words,n));
    if(compare_compute)assert(n==compute_words && !memcmp(words,compute_reference,n*4));
    else {compute_reference=malloc(n*4);assert(compute_reference);memcpy(compute_reference,words,n*4);compute_words=n;}
    return false;
}
#define vk_shader_module_acquire mock_module_acquire
void vk_shader_module_release(VkShaderModuleLease* p){assert(!p->entry);}
VkResult vk_pipeline_compute_build(VkShaderModule m,VkPipelineLayout l,const uint32_t local[3],VkPipeline* p){(void)m;(void)l;(void)local;(void)p;return VK_ERROR_INITIALIZATION_FAILED;}
void vk_pipeline_recipe_compute(VkPipelineRecipe* p,uint64_t k,const uint32_t local[3]){memset(p,0,sizeof *p);p->shader[0]=k;memcpy(p->local,local,12);}
bool vk_pipeline_warm_take(const VkPipelineRecipe* r,VkPipelineWarmProduct* p){(void)r;(void)p;return false;}
void vk_pipeline_warm_record(const VkPipelineRecipe* r){(void)r;}
const char* rt_data_root(void){return ".";}
'''
test = r'''
static uint8_t blobs[2][0xc0],controls[2][0x880];
static void put32(uint8_t* p,uint32_t n){memcpy(p,&n,4);}
static void prepare_input(void){
    for(unsigned i=0;i<2;++i){
        put32(controls[i],0x98761234);put32(controls[i]+0x20,0x880);put32(controls[i]+0x704,0xc0);
        put32(controls[i]+0x700,0xc0);put32(controls[i]+0x708,0x30);put32(controls[i]+0x714,i);
        uint64_t code[]={0,0x0103f00000070000ull,0x0103f00000070001ull,0x0103f00000070002ull,
                         0,0x0103f80000070003ull,0xe30000000007000full,0xe2400fffff87000full};
        memcpy(blobs[i]+0x80,code,sizeof code);
    }
    put32(blobs[1]+0x78,15);put32(blobs[1]+0x7c,2);
}
static Prog init(void){
    Prog p={0};p.vkey.glsl=p.fkey.glsl=1;p.vkey.gather_bias_bits=p.fkey.gather_bias_bits=8;
    for(unsigned i=0;i<2;++i){
        p.code[i]=(uintptr_t)blobs[i];p.cmax[i]=sizeof blobs[i];
        p.inputs[i]=(NvnShaderStage){.stage=i,.code_addr=p.code[i],.control=controls[i],.const_offset=sizeof blobs[i],.entry_offset=0x30};
    }
    return p;
}
static void dispose(Prog* p){graphics_program_release(p);maxwell_free(p->vi);maxwell_free(p->fi);}
int main(int argc,char** argv){
    assert(argc==2 && !chdir(argv[1]));
    char* compiler=strdup(getenv("LARECOMP_NATIVE_LIB_DIR"));assert(compiler);
    prepare_input();unsetenv("LARECOMP_SHADER_ARTIFACTS");unsetenv("LARECOMP_VK_OPT");
    Prog first=init();translate_program(&first);
    assert(first.state==1 && translated==2 && compile_calls==1 && saved_sources==2);
    assert(first.vi->msl && first.fi->msl && first.fi->writes_depth && first.fs_out_mask==15);
    uint64_t keys[]={first.source_keys[0],first.source_keys[1]};dispose(&first);
    compare=true;
    // Different host addresses holding the same complete input still hit the
    // prepared cache; the shader compiler and translator are both unavailable.
    uint8_t copies[2][0xc0];memcpy(copies,blobs,sizeof copies);
    setenv("LARECOMP_NATIVE_LIB_DIR","/compiler-unavailable",1);fail_stage=0;
    translated=saved_sources=0;
    Prog cached=init();
    for(unsigned i=0;i<2;++i){cached.code[i]=(uintptr_t)copies[i];cached.inputs[i].code_addr=cached.code[i];}
    translate_program(&cached);
    assert(cached.state==1 && !translated && !saved_sources && !cached.vi->msl && !cached.fi->msl);
    assert(cached.fi->writes_depth && cached.fs_out_mask==15 && cached.source_keys[0]==keys[0] && cached.source_keys[1]==keys[1]);
    assert(!cached.prepared_spirv[0] && !cached.prepared_spirv[1]);dispose(&cached);fail_stage=-1;
    // Missing metadata triggers actual translation even when SPIR-V exists;
    // strict source-cache hits still work without opening shaderc.
    setenv("LARECOMP_SHADER_ARTIFACTS","0",1);translated=0;
    Prog legacy=init();translate_program(&legacy);assert(legacy.state==1 && translated==2);dispose(&legacy);
    unsetenv("LARECOMP_SHADER_ARTIFACTS");
    // A source-export diagnostic retains the real GLSL path.
    setenv("LARECOMP_DUMP_VERTEX_INPUT","1",1);translated=saved_sources=0;
    Prog diagnostic=init();translate_program(&diagnostic);assert(diagnostic.state==1 && translated==2 && saved_sources==2);dispose(&diagnostic);
    unsetenv("LARECOMP_DUMP_VERTEX_INPUT");
    // An allocation failure in either stage must publish failure, never
    // dereference a NULL ShaderInfo or silently claim a completed program.
    setenv("LARECOMP_SHADER_ARTIFACTS","0",1);
    for(int stage=0;stage<2;++stage){fail_stage=stage;Prog bad=init();translate_program(&bad);assert(bad.state==2);dispose(&bad);}
    fail_stage=-1;unsetenv("LARECOMP_SHADER_ARTIFACTS");
    setenv("LARECOMP_NATIVE_LIB_DIR",compiler,1);compare=false;translated=0;
    Prog clipped=init();clipped.vkey.flip_y=true;clipped.vkey.depth_minus_one=true;
    translate_program(&clipped);assert(clipped.state==1 && translated==1 && clipped.source_keys[0]!=keys[0] && clipped.source_keys[1]==keys[1]);dispose(&clipped);
    // Actual compute_program + build_compute: the input includes the NVN
    // prefix and workgroup metadata, and the stage's translation starts at
    // entry_offset. Driver module failure is deliberate and must still clean
    // up the transferred SPIR-V exactly once.
    setenv("LARECOMP_NATIVE_LIB_DIR",compiler,1);
    put32(controls[0]+0x714,5);put32(controls[0]+0x748,4);
    put32(controls[0]+0x708,0x80);
    NvnProgram np={0};np.generation=1;
    np.stages[5]=(NvnShaderStage){.stage=5,.code_addr=(uintptr_t)blobs[0],.control=controls[0],.const_offset=sizeof blobs[0],.entry_offset=0x80};
    translated=0;CsProg* cs=compute_program(&np);
    assert(cs->p.state==2 && translated==1 && cs->si->msl && cs->local[0]==4 && !cs->p.prepared_spirv[0]);
    setenv("LARECOMP_NATIVE_LIB_DIR","/compiler-unavailable",1);
    uint64_t ckey=cs->p.source_keys[0];compare_compute=true;fail_stage=5;translated=0;
    np.stages[5].code_addr=(uintptr_t)copies[0];++np.generation;
    CsProg* hit=compute_program(&np);
    assert(hit!=cs && hit->p.state==2 && !translated && !hit->si->msl && hit->local[0]==4 && hit->p.source_keys[0]==ckey && !hit->p.prepared_spirv[0]);
    fail_stage=-1;
    // Prefix/control changes must invalidate the artifact identity; keep the
    // code unchanged to isolate the input metadata.
    put32(controls[0]+0x748,8);++np.generation;translated=0;
    CsProg* changed=compute_program(&np);assert(translated==1 && changed->local[0]==8);
    for(int i=0;i<g_cs_n;++i){dispose(&g_cs_tab[i].cp->p);free(g_cs_tab[i].cp);}
    free(g_cs_tab);free(compute_reference);free(compiler);shader_code_reset(&shader_code_state);
    for(unsigned i=0;i<2;++i)free(reference[i]);
    puts("PASS: actual graphics handoff bypasses translation/compiler on artifacts, preserves SPIR-V/depth/output, exports real sources and handles failed stages");
}
'''
unit=out/'graphics.c'
cs_type=source[source.index('typedef struct { Prog p; ShaderInfo *si;'):source.index('\n\nstatic void build_compute')]
cs_table=source[source.index('static struct CsEnt'):source.index('static void shader_programs_destroy(void) {')]
unit.write_text(preamble+program+function('static void graphics_program_release(')+stubs+
                function('static void compile_program(')+helpers+cs_type+function('static void build_compute(')+
                cs_table+function('static CsProg *compute_program(')+test)
flags=['-O1','-g','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
       '-DTOTK_DIAGNOSTICS=1','-I'+str(root),'-I'+str(root/'android/deps/out/include')]
subprocess.run(['clang',*flags,'-c',str(unit),'-o',str(out/'graphics.o')],check=True)
subprocess.run(['clang++','-fsanitize=address,undefined',str(out/'graphics.o'),
                str(root/'runtime/nvn/vulkan/shader_artifact.cpp'),str(root/'runtime/nvn/vulkan/shader_artifact_storage.cpp'),'-std=c++17',*flags[5:],
                str(out.parent/'shaders.o'),str(out.parent/'translate.o'),str(out.parent/'digest.o'),
                str(out.parent/'compile_budget.o'),
                str(out.parent/'shader_keys.o'),
                '-Wl,--gc-sections','-lzstd','-ldl','-lpthread','-lvulkan','-o',str(out/'graphics')],check=True)
subprocess.run([str(out/'graphics'),str(out)],check=True,timeout=30)
