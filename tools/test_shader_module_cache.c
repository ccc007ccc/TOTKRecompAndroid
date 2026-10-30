#include "../runtime/nvn/vulkan/shader_module_cache.h"
#include "../runtime/nvn/vulkan/compile_budget.h"
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
VkG g_vk;
static _Atomic unsigned loaded,reflected,created,destroyed,entered,gate;
static _Atomic bool fail;
static _Atomic bool use_budget,cancel_background,release_slots,background_ok,foreground_ok;
static _Atomic unsigned held;
static const uint32_t vs[]={0x07230203,0x10000,0,3,0,0x4000f,0,1,0};
uint32_t *vk_shader_cache_load_key(uint64_t k,bool base,size_t *n){(void)k;assert(!base);++loaded;*n=9;uint32_t *p=malloc(sizeof vs);memcpy(p,vs,sizeof vs);return p;}
int vk_reflect(const uint32_t *s,size_t n,VkBindingInfo *out,int max){assert(n>=9 && max>=1 && s[0]==vs[0]);++reflected;out[0]=(VkBindingInfo){0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};return 1;}
VkShaderModule vk_module(const uint32_t *s,size_t n){
    assert(s && n>=9);++created;entered=1;
    if(use_budget && !vk_compile_budget_acquire())return VK_NULL_HANDLE;
    while(!gate)usleep(100);VkShaderModule m=fail?VK_NULL_HANDLE:(VkShaderModule)malloc(1);
    if(use_budget)vk_compile_budget_release();return m;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule m,const VkAllocationCallbacks *a){assert(d==(VkDevice)1 && m);(void)a;++destroyed;free((void*)m);}
static VkShaderModuleLease leases[130];
static void *acquire(void *arg){size_t i=(size_t)arg;assert(vk_shader_module_acquire(123,0,NULL,0,&leases[i]));return NULL;}
static bool cancelled(void *p){(void)p;return cancel_background;}
static void *holder(void *p){(void)p;assert(vk_compile_budget_acquire());++held;while(!release_slots)usleep(100);vk_compile_budget_release();return NULL;}
static void *background_builder(void *p){(void)p;vk_compile_budget_background(true);vk_compile_budget_checks(cancelled,NULL,NULL);VkShaderModuleLease value;
    background_ok=vk_shader_module_acquire(5000,0,NULL,0,&value);if(background_ok)vk_shader_module_release(&value);return NULL;}
static void *foreground_builder(void *p){(void)p;VkShaderModuleLease value;
    foreground_ok=vk_shader_module_acquire(5000,0,NULL,0,&value);if(foreground_ok)vk_shader_module_release(&value);return NULL;}
int main(void){
    g_vk.dev=(VkDevice)1;pthread_t threads[12];
    for(size_t i=0;i<12;++i)assert(!pthread_create(&threads[i],NULL,acquire,(void*)i));
    while(!entered)usleep(100);gate=1;
    for(unsigned i=0;i<12;++i)pthread_join(threads[i],NULL);
    assert(loaded==1 && reflected==1 && created==1);
    for(unsigned i=0;i<12;++i)assert(leases[i].module==leases[0].module && leases[i].count==1 && leases[i].bindings[0].binding==1);
    vk_shader_module_cache_reset();assert(!destroyed && !vk_shader_module_cache_stats().entries);
    for(unsigned i=0;i<12;++i)vk_shader_module_release(&leases[i]);assert(destroyed==1);
    fail=true;assert(!vk_shader_module_acquire(456,0,NULL,0,&leases[0]));fail=false;
    assert(vk_shader_module_acquire(456,0,NULL,0,&leases[0]));vk_shader_module_release(&leases[0]);vk_shader_module_cache_reset();
    assert(!vk_shader_module_acquire(789,1,vs,9,&leases[0])); // stage mismatch never reaches the driver
    unsigned before=destroyed;
    for(unsigned i=0;i<130;++i)assert(vk_shader_module_acquire(1000+i,0,vs,9,&leases[i]));
    VkShaderModuleCacheStats stats=vk_shader_module_cache_stats();assert(stats.entries==128 && stats.bytes==128*sizeof vs);
    for(unsigned i=0;i<130;++i)vk_shader_module_release(&leases[i]);assert(destroyed==before+2);
    assert(vk_shader_module_acquire(2000,0,vs,9,&leases[0]));assert(vk_shader_module_cache_stats().entries==128);
    vk_shader_module_release(&leases[0]);vk_shader_module_cache_reset();assert(destroyed==before+131);
    vk_shader_module_cache_reset();
    size_t large_words=1u<<22;uint32_t *large=malloc(large_words*4);assert(large);
    memcpy(large,vs,sizeof vs);for(size_t i=9;i<large_words;++i)large[i]=0x10000; // framing-only mock driver payload
    for(unsigned i=0;i<5;++i)assert(vk_shader_module_acquire(3000+i,0,large,large_words,&leases[i]));
    assert(vk_shader_module_cache_stats().bytes==64u*1024*1024 && !leases[4].spirv && leases[4].module);
    for(unsigned i=0;i<5;++i)vk_shader_module_release(&leases[i]);
    assert(vk_shader_module_acquire(4000,0,large,large_words,&leases[0]));assert(vk_shader_module_cache_stats().bytes==64u*1024*1024);
    vk_shader_module_release(&leases[0]);vk_shader_module_cache_reset();free(large);
    // A foreground borrower promotes the background module dependency itself,
    // before any pipeline recipe exists. Cancelling that builder must retry in
    // the foreground, never turn a ready shader into a failed/lost program.
    use_budget=true;entered=0;vk_compile_budget_loading(true);unsigned limit=vk_compile_budget_stats().limit;
    pthread_t slots[4],back,front;
    for(unsigned i=0;i<limit;++i)assert(!pthread_create(&slots[i],NULL,holder,NULL));while(held<limit)usleep(100);
    assert(!pthread_create(&back,NULL,background_builder,NULL));while(!entered)usleep(100);
    assert(!pthread_create(&front,NULL,foreground_builder,NULL));while(!vk_compile_budget_stats().waiting)usleep(100);
    cancel_background=true;vk_compile_budget_wake();pthread_join(back,NULL);assert(!background_ok);
    release_slots=true;for(unsigned i=0;i<limit;++i)pthread_join(slots[i],NULL);pthread_join(front,NULL);
    assert(foreground_ok && !vk_compile_budget_stats().active);vk_shader_module_cache_reset();
    puts("PASS: per-key coalescing, stage checks, entry/byte bounds, transient storage, pinned reset, dependency promotion and foreground retry after background cancellation");
}
