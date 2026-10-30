"""Execute production program destruction with counted driver handles under ASan."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / '.work/shader-lifetimes').resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
device = (root / 'runtime/nvn/vulkan/device.c').read_text().split('void backend_reset(void) {',1)[1].split('void backend_guest_memory_reset',1)[0]
assert device.index('vk_shader_jobs_drain();') < device.index('renderer_flush();') < device.index('vk_exec_reset();')
warm = (root / 'runtime/nvn/vulkan/pipeline_warm.c').read_text()
def block(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]
def function(text, start):
    a = text.index(start)
    return text[a:text.index('\n}\n', a) + 3]
program = block(source, 'typedef struct {\n    ShaderInfo *vi', '} Prog;') + '} Prog;\n'
pipe = block(source, 'typedef struct { Prog *prog; VkPipelineState state; } PipeKey;', '// 8 bytes a step')
compute = block(source, 'typedef struct { Prog p; ShaderInfo *si;', '\n\nstatic void build_compute')
table = block(source, 'static struct CsEnt', 'static void shader_programs_destroy(void) {')
preamble = r'''
#include <volk.h>
#include "runtime/nvn/vulkan/shaders.h"
#include "runtime/nvn/vulkan/pipeline_warm.h"
#include "runtime/nvn/vulkan/shader_object_cache.h"
#include "runtime/nvn/vulkan/shader_code_state.h"
#include "runtime/nvn/vulkan/shader_module_cache.h"
#include "runtime/nvn/vulkan/descriptor_update.h"
#include "runtime/nvn/maxwell/shader.h"
#include <assert.h>
#include <stdatomic.h>
#define MAX_BIND 96
VkG g_vk;
static ShaderCodeState shader_code_state;
static ShaderObjectTable prog_tab={.initial_capacity=4};
static bool pipeline_dirty;
static int g_cs_n;
static unsigned live, modules, pipelines, layouts, sets, infos;
static unsigned templates;
typedef struct { uint8_t regular[MAX_BIND], deferred[MAX_BIND]; int nregular, ndeferred; } DescriptorPlan;
void rt_fatal_trace(void){abort();}
void rt_fatal_message(const char* format,...){(void)format;abort();}
static void* handle(void){++live;return malloc(1);}
static void release(void* p){assert(p && live);--live;free(p);}
static void module(VkDevice d,VkShaderModule p,const VkAllocationCallbacks* a){(void)d;(void)a;++modules;release((void*)p);}
static void pipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks* a){(void)d;(void)a;++pipelines;release((void*)p);}
static void layout(VkDevice d,VkPipelineLayout p,const VkAllocationCallbacks* a){(void)d;(void)a;++layouts;release((void*)p);}
static void set(VkDevice d,VkDescriptorSetLayout p,const VkAllocationCallbacks* a){(void)d;(void)a;++sets;release((void*)p);}
PFN_vkDestroyShaderModule vkDestroyShaderModule=module;
PFN_vkDestroyPipeline vkDestroyPipeline=pipeline;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout=layout;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout=set;
#if defined(__ANDROID__) && TOTK_DIAGNOSTICS
static void destroy_template(VkDevice d,VkDescriptorUpdateTemplate p,const VkAllocationCallbacks* a){(void)d;(void)a;++templates;release((void*)p);}
PFN_vkCreateDescriptorUpdateTemplate vkCreateDescriptorUpdateTemplate;
PFN_vkDestroyDescriptorUpdateTemplate vkDestroyDescriptorUpdateTemplate=destroy_template;
PFN_vkUpdateDescriptorSetWithTemplate vkUpdateDescriptorSetWithTemplate;
#endif
void maxwell_free(ShaderInfo* p){if(p){++infos;free(p);}}
void vk_shader_module_release(VkShaderModuleLease* p){assert(!p->entry);}
void vk_shader_module_cache_reset(void){}
static void program_cache_reset(void){shader_object_clear(&prog_tab);}
'''
test = r'''
static void populate(Prog* p){
    // A translated program can fail or reset before its prepared buffers have
    // been consumed by the compiler. These remain owned by the program.
    for(unsigned i=0;i<2;++i){p->prepared_words[i]=16;p->prepared_spirv[i]=calloc(16,4);assert(p->prepared_spirv[i]);}
    p->vs=(VkShaderModule)handle();p->fs=(VkShaderModule)handle();
    p->layout=(VkPipelineLayout)handle();
    for(unsigned i=0;i<2;++i)p->set_layout[i]=(VkDescriptorSetLayout)handle();
#if defined(__ANDROID__) && TOTK_DIAGNOSTICS
    for(unsigned i=0;i<2;++i){p->descriptor_updates[i]=calloc(1,sizeof(VkDescriptorUpdate));p->descriptor_updates[i]->update=(VkDescriptorUpdateTemplate)handle();}
#endif
    p->vi=calloc(1,sizeof *p->vi);p->fi=calloc(1,sizeof *p->fi);
}
static VkPipelineWarmProduct warmed(void){return (VkPipelineWarmProduct){(VkPipeline)handle(),(VkPipelineLayout)handle(),{(VkDescriptorSetLayout)handle(),(VkDescriptorSetLayout)handle()}};}
int main(void){
    for(unsigned restart=0;restart<100;++restart){
        Prog* p=calloc(1,sizeof *p);populate(p);assert(shader_object_put(&prog_tab,1,p));
        for(unsigned i=0;i<2;++i){
            Pipe* q=calloc(1,sizeof *q);q->k.prog=p;
            if(i){q->warmed=warmed();q->pipe=q->warmed.pipeline;}else q->pipe=(VkPipeline)handle();
            assert(shader_object_put(&pipe_tab,i+1,q));last_pipeline=q;
        }
        g_cs_tab=calloc(2,sizeof *g_cs_tab);g_cs_n=g_cs_capacity=2;
        for(unsigned i=0;i<2;++i){
            CsProg* c=calloc(1,sizeof *c);populate(&c->p);free(c->p.fi);c->p.fi=NULL;c->si=c->p.vi;
            if(i){c->warmed=warmed();c->pipe=c->warmed.pipeline;}else c->pipe=(VkPipeline)handle();
            g_cs_tab[i].cp=c;
        }
        shader_code_range(&shader_code_state,0x1000,32);
        shader_code_range(&shader_code_state,0x3000,32);
        shader_code_written(&shader_code_state,0x2000,32);assert(!shader_code_state.epoch);
        shader_code_written(&shader_code_state,0x3010,32);assert(shader_code_state.epoch==1);
        shader_programs_destroy();
        assert(!live && !last_pipeline && !prog_tab.entries && !pipe_tab.entries && !g_cs_tab && !g_cs_n && !shader_code_state.ranges);
        shader_programs_destroy(); // repeat reset owns nothing
    }
    assert(pipelines==400 && modules==600 && layouts==500 && sets==1000 && infos==400);
#if defined(__ANDROID__) && TOTK_DIAGNOSTICS
    assert(templates==600);
#endif
    puts("PASS: 100 reboots release graphics/compute programs, normal and warmed pipelines/layouts exactly once");
}
'''
unit = out / 'lifetimes.c'
unit.write_text(preamble + program + pipe + compute + table +
                function(source, 'static void graphics_program_release(') +
                function(warm, 'void vk_pipeline_warm_discard(') +
                function(source, 'static void shader_programs_destroy(void) {') + test)
subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-I'+str(root),
                '-I'+str(root/'android/deps/out/include'), '-I'+str(root/'android/deps/out/volk'),
                str(unit), '-o', str(out/'test-lifetimes')], check=True)
subprocess.run([str(out/'test-lifetimes')], check=True, timeout=15)
subprocess.run(['clang', '-O1', '-g', '-D__ANDROID__', '-DTOTK_DIAGNOSTICS=1', '-fsanitize=address,undefined', '-I'+str(root),
                '-I'+str(root/'android/deps/out/include'), '-I'+str(root/'android/deps/out/volk'), str(unit),
                str(root/'runtime/nvn/vulkan/descriptor_update.c'), '-o', str(out/'test-lifetimes-diagnostic')], check=True)
subprocess.run([str(out/'test-lifetimes-diagnostic')], check=True, timeout=15)
