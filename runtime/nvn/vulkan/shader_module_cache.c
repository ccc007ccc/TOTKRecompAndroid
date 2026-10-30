#include "shader_module_cache.h"
#include "shader_cache.h"
#include "shader_prepare.h"
#include "shader_spirv.h"
#include "compile_budget.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define MODULE_MAX 128
#define MODULE_BYTES (64u*1024*1024)
#define MODULE_BINDS 96
typedef struct Entry {
    struct Entry *next;
    VkDevice device;
    uint64_t key, touched;
    int stage;
    unsigned references, count;
    bool building, retained, cancelled, missing;
    _Atomic bool urgent;
    uint32_t *spirv;
    size_t words, retained_bytes;
    VkShaderModule module;
    VkBindingInfo bindings[MODULE_BINDS];
} Entry;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static Entry *entries;
static unsigned count;
static size_t bytes;
static uint64_t use_clock, hits, builds;
static void destroy(Entry *e) {
    if(e->module)vkDestroyShaderModule(e->device,e->module,NULL);
    free(e->spirv);free(e);
}
static void unlink_entry(Entry *e) { // lock held
    Entry **at=&entries;while(*at && *at!=e)at=&(*at)->next;
    if(*at){*at=e->next;--count;bytes-=e->retained_bytes;e->retained_bytes=0;}
    e->retained=false;
}
static bool evict(void) { // only idle entries, never builders or borrowers
    Entry *old=NULL;
    for(Entry *e=entries;e;e=e->next)if(!e->references && !e->building && (!old || e->touched<old->touched))old=e;
    if(!old)return false;
    unlink_entry(old);destroy(old);return true;
}
static void lease(Entry *e,VkShaderModuleLease *out) {
    *out=(VkShaderModuleLease){e,e->module,e->spirv,e->words,e->bindings,e->count};
}
static bool module_urgent(void *e) {return atomic_load(&((Entry*)e)->urgent);}
bool vk_shader_module_acquire(uint64_t key,int stage,const uint32_t *words,size_t n,VkShaderModuleLease *out) {
    if(!out)return false;memset(out,0,sizeof *out);
    if(!key || !g_vk.dev || (stage!=0 && stage!=1 && stage!=5))return false;
    pthread_mutex_lock(&lock);
    for(Entry *e=entries;e;e=e->next)if(e->key==key && e->stage==stage && e->device==g_vk.dev) {
        ++e->references;e->touched=++use_clock;
        if(!vk_compile_budget_is_background())e->urgent=true;
        while(e->building)pthread_cond_wait(&changed,&lock);
        bool ok=e->module!=VK_NULL_HANDLE;
        bool retry=!ok && !vk_compile_budget_is_background() && (e->cancelled || (e->missing && words));
        if(ok){lease(e,out);++hits;}
        else if(!--e->references){unlink_entry(e);destroy(e);}
        pthread_mutex_unlock(&lock);return retry?vk_shader_module_acquire(key,stage,words,n,out):ok;
    }
    Entry *e=calloc(1,sizeof *e);
    if(!e){pthread_mutex_unlock(&lock);return false;}
    e->device=g_vk.dev;e->key=key;e->stage=stage;e->references=1;e->building=true;e->touched=++use_clock;
    while(count>=MODULE_MAX && evict()){}
    // If every retained object is pinned, the required operation still works
    // with a transient lease. Cache growth remains bounded.
    e->retained=count<MODULE_MAX;
    if(e->retained){e->next=entries;entries=e;++count;}
    pthread_mutex_unlock(&lock);
    if(words && n && n<=MODULE_BYTES/4) {
        e->spirv=malloc(n*4);if(e->spirv){memcpy(e->spirv,words,n*4);e->words=n;}
    } else if(!words)e->spirv=vk_shader_cache_load_key(key,false,&e->words);
    e->missing=!e->spirv || !vk_shader_spirv_stage(e->spirv,e->words,stage);
    if(!e->missing) {
        e->count=vk_reflect(e->spirv,e->words,e->bindings,MODULE_BINDS);
        VkCompileBudgetDependency previous=vk_compile_budget_dependency((VkCompileBudgetDependency){module_urgent,e});
        e->module=vk_module(e->spirv,e->words);
        e->cancelled=!e->module && vk_compile_budget_cancelled();
        vk_compile_budget_dependency(previous);
    }
    pthread_mutex_lock(&lock);++builds;
    if(e->retained) {
        // words become accounted only on publication; remove the entry before
        // evicting so its byte count cannot underflow during unlink.
        size_t needed=e->words*4;
        while(needed>MODULE_BYTES-bytes && evict()){}
        if(needed>MODULE_BYTES-bytes)unlink_entry(e);
        else {bytes+=needed;e->retained_bytes=needed;}
    }
    e->building=false;bool ok=e->module!=VK_NULL_HANDLE;
    if(!ok && e->retained)unlink_entry(e);
    if(!e->retained){free(e->spirv);e->spirv=NULL;}
    if(ok)lease(e,out);
    else if(!--e->references){if(e->retained)unlink_entry(e);destroy(e);}
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);return ok;
}
void vk_shader_module_release(VkShaderModuleLease *value) {
    if(!value || !value->entry)return;
    pthread_mutex_lock(&lock);Entry *e=value->entry;
    if(!--e->references && !e->retained)destroy(e);
    memset(value,0,sizeof *value);pthread_mutex_unlock(&lock);
}
void vk_shader_module_cache_reset(void) {
    pthread_mutex_lock(&lock);
    while(entries){Entry *e=entries;unlink_entry(e);if(!e->references)destroy(e);}
    pthread_mutex_unlock(&lock);
}
VkShaderModuleCacheStats vk_shader_module_cache_stats(void) {
    pthread_mutex_lock(&lock);VkShaderModuleCacheStats s={hits,builds,count,bytes};pthread_mutex_unlock(&lock);return s;
}
