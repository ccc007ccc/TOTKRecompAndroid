// Real metadata files and threads, with a fake driver callback. The separate GPU
// test exercises the shared pipeline builder and binding of a warmed handle.
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/pipeline_warm.c"
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
const char *vk_shader_cache_namespace(void) { return "test-compiler"; }
static _Atomic unsigned destroyed,destroyed_layouts,destroyed_sets,entered,gate,stopped,take_done;
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a) {
    (void)a;assert(d==g_vk.dev && p);++destroyed;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout p,const VkAllocationCallbacks *a) {(void)a;assert(d==g_vk.dev && p);++destroyed_layouts;}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout p,const VkAllocationCallbacks *a) {(void)a;assert(d==g_vk.dev && p);++destroyed_sets;}
static VkPipelineRecipe recipe(unsigned key) {
    VkPipelineState state={0};state.ncolor=1;state.color[0]=VK_FORMAT_R8G8B8A8_UNORM;state.samples=1;
    state.topo_class=2;state.poly_fill=1;state.write_mask[0]=15;
    VkPipelineRecipe out;vk_pipeline_recipe_make(&out,key+1,9,&state);return out;
}
static void product(const VkPipelineRecipe *r,VkPipelineWarmProduct *p) {
    p->pipeline=(VkPipeline)(uintptr_t)r->shader[0];p->layout=(VkPipelineLayout)(uintptr_t)1;
    p->sets[0]=(VkDescriptorSetLayout)(uintptr_t)2;p->sets[1]=(VkDescriptorSetLayout)(uintptr_t)3;
}
static VkResult build(const VkPipelineRecipe *r,VkPipelineWarmProduct *p) {
    if(r->shader[0]==2)return VK_NOT_READY;
    if(r->shader[0]==3)return VK_ERROR_INITIALIZATION_FAILED;
    product(r,p);return VK_SUCCESS;
}
static VkResult gated_build(const VkPipelineRecipe *r,VkPipelineWarmProduct *p) {
    ++entered;while(!gate)usleep(100);product(r,p);return VK_SUCCESS;
}
static void *take_thread(void *arg) {VkPipelineWarmProduct p;assert(!vk_pipeline_warm_take(arg,&p));take_done=1;return NULL;}
static void *stop_thread(void *arg) {(void)arg;vk_pipeline_warm_stop();stopped=1;return NULL;}
static void reload(VkPipelineWarmBuild callback) {initialized=false;vk_pipeline_warm_start(callback);}
static void copy_file(const char *from,const char *to) {
    FILE *a=fopen(from,"rb"),*b=fopen(to,"wb");assert(a && b);char data[4096];size_t n;
    while((n=fread(data,1,sizeof data,a)))assert(fwrite(data,1,n,b)==n);
    assert(!ferror(a) && !fclose(a) && !fclose(b));
}
int main(int argc,char **argv) {
    assert(argc==2);char directory[1200];snprintf(directory,sizeof directory,"%s/recipes-XXXXXX",argv[1]);
    assert(mkdtemp(directory) && !chdir(directory));g_vk.dev=(VkDevice)(uintptr_t)1;
    g_vk.props.vendorID=0x5143;g_vk.props.deviceID=0x830;g_vk.props.driverVersion=17;
    for(unsigned i=0;i<VK_UUID_SIZE;++i)g_vk.props.pipelineCacheUUID[i]=i;
    vk_pipeline_warm_start(NULL);
    uint32_t local[3]={2,2,2};VkPipelineRecipe compute;
    vk_pipeline_recipe_compute(&compute,17,local);assert(valid_recipe(&compute));
    compute.state.samples=1;assert(!valid_recipe(&compute));
    vk_pipeline_recipe_compute(&compute,17,local);compute.local[0]=0;assert(!valid_recipe(&compute));
    for(unsigned i=0;i<3;++i){VkPipelineRecipe r=recipe(i);vk_pipeline_warm_record(&r);vk_pipeline_warm_record(&r);}
    assert(count==3 && vk_pipeline_warm_save());copy_file("cache/vk_pipeline_recipes.bin","good.bin");
    reload(build);
    for(unsigned i=0;i<10000;++i){VkPipelineWarmStats s=vk_pipeline_warm_stats();if(s.prepared+s.missing+s.failed==3)break;usleep(100);}
    VkPipelineWarmStats s=vk_pipeline_warm_stats();assert(s.prepared==1 && s.missing==1 && s.failed==1);
    VkPipelineWarmProduct p;VkPipelineRecipe r=recipe(0);assert(vk_pipeline_warm_take(&r,&p) && p.pipeline);
    vk_pipeline_warm_stop();assert(!destroyed && !destroyed_layouts && !destroyed_sets);
    vk_pipeline_warm_discard(&p);assert(destroyed==1 && destroyed_layouts==1 && destroyed_sets==2);
    // A failed metadata replacement preserves the preceding complete file.
    r=recipe(4);vk_pipeline_warm_record(&r);assert(!mkdir("cache/vk_pipeline_recipes.bin.part",0755));
    assert(!vk_pipeline_warm_save());assert(!rmdir("cache/vk_pipeline_recipes.bin.part"));
    reload(NULL);assert(count==3);
    // Driver, source/build identity and frame integrity are all checked before use.
    ++g_vk.props.driverVersion;reload(NULL);assert(!count);--g_vk.props.driverVersion;
    reload(NULL);assert(count==3);
    FILE *f=fopen("cache/vk_pipeline_recipes.bin","r+b");assert(f && !fseek(f,-1,SEEK_END));int byte=fgetc(f);
    assert(byte!=EOF && !fseek(f,-1,SEEK_CUR) && fputc(byte^1,f)!=EOF && !fclose(f));
    reload(NULL);assert(!count);copy_file("good.bin","cache/vk_pipeline_recipes.bin");
    f=fopen("cache/vk_pipeline_recipes.bin","ab");assert(f && fputc(1,f)!=EOF && !fclose(f));
    reload(NULL);assert(!count);copy_file("good.bin","cache/vk_pipeline_recipes.bin");
    assert(!truncate("cache/vk_pipeline_recipes.bin",sizeof(RecipeHeader)+sizeof(VkPipelineRecipe)-1));
    reload(NULL);assert(!count);copy_file("good.bin","cache/vk_pipeline_recipes.bin");
    f=fopen("cache/vk_pipeline_recipes.bin","r+b");RecipeHeader h;
    assert(f && fread(&h,sizeof h,1,f)==1);h.count=RECIPE_MAX+1;
    rewind(f);assert(fwrite(&h,sizeof h,1,f)==1 && !fclose(f));
    reload(NULL);assert(!count);copy_file("good.bin","cache/vk_pipeline_recipes.bin");
    f=fopen("cache/vk_pipeline_recipes.bin","r+b");assert(f && fread(&h,sizeof h,1,f)==1);h.compiler[0]^=1;
    rewind(f);assert(fwrite(&h,sizeof h,1,f)==1 && !fclose(f));
    reload(NULL);assert(!count);copy_file("good.bin","cache/vk_pipeline_recipes.bin");
    // Exercise render lookup racing with lifecycle cancellation/freeing entries.
    reload(NULL);for(unsigned i=3;i<100;++i){r=recipe(i);vk_pipeline_warm_record(&r);}assert(vk_pipeline_warm_save());
    vk_pipeline_warm_start(gated_build);while(!entered)usleep(100);
    r=recipe(0);assert(!vk_pipeline_warm_take(&r,&p)); // queued, dispatched through the normal draw path
    VkPipelineRecipe building=recipe(99);pthread_t taker,stopper;
    assert(!pthread_create(&taker,NULL,take_thread,&building));
    for(unsigned i=0;i<10000;++i){pthread_mutex_lock(&lock);bool waiting=readers!=0;pthread_mutex_unlock(&lock);if(waiting)break;usleep(100);}
    assert(!take_done);
    vk_pipeline_warm_cancel_async(); // must return while the driver callback is still blocked
    while(!take_done)usleep(100);
    assert(!stopped && !gate && !pthread_create(&stopper,NULL,stop_thread,NULL));
    gate=1;
    assert(!pthread_join(taker,NULL) && !pthread_join(stopper,NULL) && stopped);
    setenv("LARECOMP_PIPELINE_WARM","0",1);vk_pipeline_warm_start(build);assert(!entry_count && count==100);
    vk_pipeline_warm_stop();
    for(unsigned i=100;i<RECIPE_MAX+5;++i){r=recipe(i);vk_pipeline_warm_record(&r);}
    assert(count==RECIPE_MAX && records[0].shader[0]==6 && records[count-1].shader[0]==RECIPE_MAX+5);
    assert(vk_pipeline_warm_save());reload(NULL);assert(count==RECIPE_MAX);
    puts("PASS: recipe canonicalization/dedup, driver identity, corruption/trailing bytes, atomic write failure, cached/missing/failed builds, handle ownership, queued demand and cancellation race");
}
