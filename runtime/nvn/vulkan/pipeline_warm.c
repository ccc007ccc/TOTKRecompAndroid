// Device-local, bounded recipes collected from successful Vulkan draws. These
// contain source keys and fixed-function state, never game code or raw handles.
#include "pipeline_warm.h"
#include "shader_prepare.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifndef SHADER_BUILD_ID
#define SHADER_BUILD_ID "development"
#endif

#define RECIPE_MAX 8192
enum { QUEUED, BUILDING, READY, TAKEN, SKIPPED };
typedef struct { VkPipelineRecipe recipe; uint64_t hash; VkPipelineWarmProduct product; unsigned state; } WarmEntry;
typedef struct {
    uint64_t magic, digest;
    uint32_t version, endian, record_bytes, count, vendor, device, driver, features;
    uint8_t uuid[VK_UUID_SIZE];
    char build[65], compiler[65];
} RecipeHeader;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER, save_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static VkPipelineRecipe *records;
static uint64_t *record_hashes;
static unsigned count;
static uint64_t generation, saved_generation;
static bool initialized, cancel;
static RecipeHeader identity;
static WarmEntry *entries;
static unsigned entry_count, cursor;
static unsigned readers;
static pthread_t threads[64];
static unsigned thread_count;
static VkPipelineWarmBuild builder;
static VkPipelineWarmStats stats;

static uint64_t hash_bytes(const void *data,size_t size) {
    uint64_t h=0xcbf29ce484222325ull;const uint8_t *p=data;
    for(size_t i=0;i<size;++i)h=(h^p[i])*0x100000001b3ull;
    return h;
}
void vk_pipeline_recipe_make(VkPipelineRecipe *out,uint64_t vertex,uint64_t fragment,const VkPipelineState *state) {
    memset(out,0,sizeof *out);out->shader[0]=vertex;out->shader[1]=fragment;
    // Copy fields explicitly so compiler padding never enters the persistent key.
#define FIELD(name) memcpy(&out->state.name,&state->name,sizeof state->name)
    FIELD(color);FIELD(depth);FIELD(stencil);FIELD(ncolor);FIELD(samples);
    FIELD(attr_fmt);FIELD(attr_off);FIELD(attr_bind);FIELD(attr_mask);
    FIELD(stride);FIELD(divisor);FIELD(used_streams);FIELD(blend);FIELD(blend_on);
    FIELD(a2c);FIELD(topo_class);FIELD(poly_fill);FIELD(depth_clamp);FIELD(prim_restart);FIELD(write_mask);
    FIELD(fx.on);FIELD(fx.cull);FIELD(fx.ff);FIELD(fx.topo);FIELD(fx.dt);FIELD(fx.dw);FIELD(fx.dop);FIELD(fx.sten);FIELD(fx.sop);
#undef FIELD
}
static bool valid_recipe(const VkPipelineRecipe *r) {
    if(!r->shader[0])return false;
    VkPipelineRecipe canonical;
    if(r->shader[1]) {
        if(!vk_pipeline_state_valid(&r->state))return false;
        vk_pipeline_recipe_make(&canonical,r->shader[0],r->shader[1],&r->state);
    } else {
        if(!vk_pipeline_local_size_valid(r->local))return false;
        vk_pipeline_recipe_compute(&canonical,r->shader[0],r->local);
    }
    return !memcmp(&canonical,r,sizeof canonical);
}
void vk_pipeline_recipe_compute(VkPipelineRecipe *out,uint64_t compute,const uint32_t local[3]) {
    memset(out,0,sizeof *out);out->shader[0]=compute;memcpy(out->local,local,sizeof out->local);
}
static RecipeHeader current_identity(void) {
    RecipeHeader h;memset(&h,0,sizeof h);h.magic=0x3152504b564b5454ull;h.version=1;h.endian=0x01020304;h.record_bytes=sizeof(VkPipelineRecipe);
    h.vendor=g_vk.props.vendorID;h.device=g_vk.props.deviceID;h.driver=g_vk.props.driverVersion;
    h.features=g_vk.dyn_render | (g_vk.eds<<1) | (g_vk.eds2<<2) | (g_vk.divisor<<3) | ((getenv("LARECOMP_NO_DYN_UBO")!=NULL)<<4);
    memcpy(h.uuid,g_vk.props.pipelineCacheUUID,sizeof h.uuid);
    snprintf(h.build,sizeof h.build,"%s",SHADER_BUILD_ID);
    snprintf(h.compiler,sizeof h.compiler,"%s",vk_shader_cache_namespace());
    return h;
}
static void read_records(void) { // lock held, called only with previous workers joined
    count=0;generation=saved_generation=0;
    FILE *f=fopen("cache/vk_pipeline_recipes.bin","rb");if(!f)return;
    RecipeHeader h;bool ok=fread(&h,sizeof h,1,f)==1;
    unsigned n=ok?h.count:0;uint64_t digest=ok?h.digest:0;h.count=0;h.digest=0;
    ok=ok && n<=RECIPE_MAX && !memcmp(&h,&identity,sizeof h);
    if(ok)ok=fread(records,sizeof *records,n,f)==n && fgetc(f)==EOF && !ferror(f) && hash_bytes(records,n*sizeof *records)==digest;
    fclose(f);
    if(ok)for(unsigned i=0;i<n;++i)if(!valid_recipe(&records[i])){ok=false;break;}
    if(!ok){LOG("[pipeline-warm] incompatible or damaged recipe file ignored\n");return;}
    count=n;for(unsigned i=0;i<count;++i)record_hashes[i]=hash_bytes(&records[i],sizeof *records);
}
static void *warm_worker(void *unused) {
    (void)unused;
#ifndef _WIN32
    setpriority(PRIO_PROCESS,0,10);
#endif
    for(;;) {
        pthread_mutex_lock(&lock);
        while(cursor<entry_count && entries[cursor].state!=QUEUED)++cursor;
        if(cancel || cursor==entry_count){pthread_mutex_unlock(&lock);break;}
        WarmEntry *entry=&entries[cursor++];entry->state=BUILDING;
        pthread_mutex_unlock(&lock);
        VkPipelineWarmProduct product={0};VkResult result=builder(&entry->recipe,&product);
        if(result!=VK_SUCCESS || !product.pipeline)vk_pipeline_warm_discard(&product);
        pthread_mutex_lock(&lock);
        if(result==VK_SUCCESS && product.pipeline){entry->product=product;entry->state=READY;++stats.prepared;}
        else {entry->state=SKIPPED;if(result==VK_NOT_READY)++stats.missing;else ++stats.failed;}
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
void vk_pipeline_warm_discard(VkPipelineWarmProduct *product) {
    if(product->pipeline)vkDestroyPipeline(g_vk.dev,product->pipeline,NULL);
    if(product->layout)vkDestroyPipelineLayout(g_vk.dev,product->layout,NULL);
    for(unsigned i=0;i<2;++i)if(product->sets[i])vkDestroyDescriptorSetLayout(g_vk.dev,product->sets[i],NULL);
    memset(product,0,sizeof *product);
}
void vk_pipeline_warm_cancel_async(void) {
    pthread_mutex_lock(&lock);cancel=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);
}
void vk_pipeline_warm_stop(void) {
    // Lifecycle calls are serialized on the event thread. Render lookup and
    // record insertion synchronize independently through lock.
    vk_pipeline_warm_cancel_async();
    for(unsigned i=0;i<thread_count;++i)pthread_join(threads[i],NULL);
    thread_count=0;
    pthread_mutex_lock(&lock);
    while(readers)pthread_cond_wait(&changed,&lock);
    for(unsigned i=0;i<entry_count;++i) {
        if(entries[i].state==QUEUED)++stats.cancelled;
        vk_pipeline_warm_discard(&entries[i].product);
    }
    if(entry_count)LOG("[pipeline-warm] prepared=%u reused=%u missing=%u failed=%u cancelled=%u\n",
        stats.prepared,stats.used,stats.missing,stats.failed,stats.cancelled);
    free(entries);entries=NULL;entry_count=cursor=0;
    pthread_mutex_unlock(&lock);
}
void vk_pipeline_warm_start(VkPipelineWarmBuild build) {
    vk_pipeline_warm_stop();
    pthread_mutex_lock(&save_lock);
    pthread_mutex_lock(&lock);
    RecipeHeader now=current_identity();
    if(!records)records=calloc(RECIPE_MAX,sizeof *records);
    if(!record_hashes)record_hashes=calloc(RECIPE_MAX,sizeof *record_hashes);
    if(!records || !record_hashes){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&save_lock);return;}
    if(!initialized || memcmp(&now,&identity,sizeof now)) {identity=now;read_records();initialized=true;}
    stats=(VkPipelineWarmStats){.loaded=count};cancel=false;builder=build;
    const char *option=getenv("LARECOMP_PIPELINE_WARM"),*general=getenv("LARECOMP_SHADER_WARM");
    bool enabled=(!option || atoi(option)!=0) && (!general || atoi(general)!=0);
    if(enabled && build && count) {
        entries=calloc(count,sizeof *entries);
        if(entries){entry_count=count;for(unsigned i=0;i<count;++i){entries[i].recipe=records[count-1-i];entries[i].hash=record_hashes[count-1-i];}}
    }
    pthread_mutex_unlock(&lock);
    pthread_mutex_unlock(&save_lock);
    if(!entry_count)return;
#ifdef _WIN32
    unsigned online=4;
#else
    long n=sysconf(_SC_NPROCESSORS_ONLN);unsigned online=n>0?(unsigned)n:1;
#endif
    if(online>64)online=64;if(online>entry_count)online=entry_count;
    for(unsigned i=0;i<online;++i)if(!pthread_create(&threads[thread_count],NULL,warm_worker,NULL))++thread_count;
    LOG("[pipeline-warm] %u recorded pipelines, %u background workers\n",entry_count,thread_count);
}
void vk_pipeline_warm_record(const VkPipelineRecipe *recipe) {
    if(!valid_recipe(recipe))return;
    uint64_t hash=hash_bytes(recipe,sizeof *recipe);
    pthread_mutex_lock(&lock);
    if(initialized) {
        unsigned at=0;for(;at<count;++at)if(record_hashes[at]==hash && !memcmp(&records[at],recipe,sizeof *recipe))break;
        if(at<count){pthread_mutex_unlock(&lock);return;}
        // Bound the history of newly observed pipeline states. A repeated hit
        // neither rewrites the file nor moves megabytes on the render thread.
        if(count==RECIPE_MAX){memmove(records,records+1,(count-1)*sizeof *records);memmove(record_hashes,record_hashes+1,(count-1)*sizeof *record_hashes);--count;}
        records[count]=*recipe;record_hashes[count++]=hash;++generation;
    }
    pthread_mutex_unlock(&lock);
}
bool vk_pipeline_warm_take(const VkPipelineRecipe *recipe,VkPipelineWarmProduct *product) {
    memset(product,0,sizeof *product);uint64_t hash=hash_bytes(recipe,sizeof *recipe);
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<entry_count;++i) {
        WarmEntry *entry=&entries[i];
        if(entry->hash!=hash || memcmp(&entry->recipe,recipe,sizeof *recipe))continue;
        // A needed queued recipe goes straight through the normal renderer pool,
        // rather than waiting behind the entire background list.
        if(entry->state==QUEUED){entry->state=SKIPPED;++stats.cancelled;break;}
        ++readers;
        while(!cancel && entry->state==BUILDING)pthread_cond_wait(&changed,&lock);
        if(!cancel && entry->state==READY){*product=entry->product;memset(&entry->product,0,sizeof entry->product);entry->state=TAKEN;++stats.used;}
        --readers;pthread_cond_broadcast(&changed);
        break;
    }
    pthread_mutex_unlock(&lock);return product->pipeline!=VK_NULL_HANDLE;
}
bool vk_pipeline_warm_save(void) {
    pthread_mutex_lock(&save_lock);pthread_mutex_lock(&lock);
    if(!initialized || generation==saved_generation){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&save_lock);return true;}
    RecipeHeader h=identity;h.count=count;uint64_t snapshot=generation;
    size_t bytes=count*sizeof *records;void *data=malloc(bytes?bytes:1);
    if(data)memcpy(data,records,bytes);
    pthread_mutex_unlock(&lock);
    bool ok=false;
    if(data) {
        h.digest=hash_bytes(data,bytes);mkdir("cache",0755);
        FILE *f=fopen("cache/vk_pipeline_recipes.bin.part","wb");
        if(f){ok=fwrite(&h,sizeof h,1,f)==1 && fwrite(data,1,bytes,f)==bytes;if(fclose(f))ok=false;}
        if(ok)ok=rename("cache/vk_pipeline_recipes.bin.part","cache/vk_pipeline_recipes.bin")==0;
        if(!ok && f)remove("cache/vk_pipeline_recipes.bin.part");
        free(data);
    }
    if(ok){pthread_mutex_lock(&lock);saved_generation=snapshot;pthread_mutex_unlock(&lock);LOG("[pipeline-warm] saved %u observed recipes\n",h.count);}
    pthread_mutex_unlock(&save_lock);return ok;
}
VkPipelineWarmStats vk_pipeline_warm_stats(void) {pthread_mutex_lock(&lock);VkPipelineWarmStats out=stats;pthread_mutex_unlock(&lock);return out;}
