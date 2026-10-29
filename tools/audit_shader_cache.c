// Read every completed cache entry through the production gameplay loader.
// No preparation session, compiler, Vulkan device, or cache writes.
#include <dirent.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/shaders.c"

VkG g_vk;
void rt_fatal_trace(void){abort();}
#ifdef __ANDROID__
void rt_fatal_message(const char *format,...){va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);abort();}
#endif
static DIR *directory;
static pthread_mutex_t listing=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t modules,bases,failed,decoded_bytes;
static double started;
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void *worker(void *unused){
    (void)unused;
    for(;;){
        char name[256];
        pthread_mutex_lock(&listing);
        struct dirent *entry=readdir(directory);
        if(entry)snprintf(name,sizeof name,"%s",entry->d_name);
        pthread_mutex_unlock(&listing);
        if(!entry)break;
        if(!strcmp(name,".") || !strcmp(name,".."))continue;
        char *end;uint64_t key=strtoull(name,&end,16);
        bool is_base=!strcmp(end,".base.bin");
        if(end-name!=16 || (!is_base && strcmp(end,".bin"))){fprintf(stderr,"unexpected cache file: %s\n",name);++failed;continue;}
        size_t words=0;uint32_t *spv=vk_shader_cache_load_key(key,is_base,&words);
        if(!spv){fprintf(stderr,"unreadable cache entry: %s\n",name);++failed;continue;}
        free(spv);
        if(is_base)++bases;
        else{atomic_fetch_add(&decoded_bytes,(uint64_t)words*4);++modules;}
        uint64_t done=atomic_load(&bases)+atomic_load(&modules);
        if(!(done%16384))printf("checked %llu modules=%llu bases=%llu failed=%llu seconds=%.1f\n",
            (unsigned long long)done,(unsigned long long)modules,(unsigned long long)bases,(unsigned long long)failed,seconds()-started);
    }
    if(prepare_library || prepare_context || prepare_compile || prepare_derive){fprintf(stderr,"compiler unexpectedly open\n");++failed;}
    return NULL;
}
int main(int argc,char **argv){
    if(argc!=4 && argc!=5){fprintf(stderr,"usage: audit_shader_cache ROOT EXPECTED_MODULES EXPECTED_BASES [NAMESPACE]\n");return 2;}
    if(chdir(argv[1])){perror("cache root");return 2;}
    uint64_t expected_modules=strtoull(argv[2],NULL,10),expected_bases=strtoull(argv[3],NULL,10);
    const char *ns=argc==5?argv[4]:vk_shader_cache_namespace();
    bool allowed=false;
    for(unsigned i=0;cache_namespaces[i];++i)if(!strcmp(ns,cache_namespaces[i]))allowed=true;
    if(!allowed){fprintf(stderr,"namespace is not compatible with this loader: %s\n",ns);return 2;}
    char path[192];snprintf(path,sizeof path,"cache/shaders/%s",ns);
    directory=opendir(path);if(!directory){perror("cache directory");return 2;}
    long online=sysconf(_SC_NPROCESSORS_ONLN);unsigned workers=online>0 && online<=256?(unsigned)online:1;
    pthread_t *threads=calloc(workers,sizeof *threads);if(!threads)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);started=seconds();unsigned opened=0;
    for(;opened<workers;++opened)if(pthread_create(&threads[opened],NULL,worker,NULL)){++failed;break;}
    for(unsigned i=0;i<opened;++i)pthread_join(threads[i],NULL);
    free(threads);closedir(directory);
    bool ok=!failed && modules==expected_modules && bases==expected_bases;
    printf("{\"scope\":\"production_cache_load_only\",\"success\":%s,\"modules\":%llu,\"bases\":%llu,\"failures\":%llu,\"decoded_module_bytes\":%llu,\"workers\":%u,\"seconds\":%.3f,\"compiler_opened\":false}\n",
        ok?"true":"false",(unsigned long long)modules,(unsigned long long)bases,(unsigned long long)failed,(unsigned long long)decoded_bytes,workers,seconds()-started);
    ShaderCacheStats hits=vk_shader_cache_stats();
    printf("{\"cache_namespace\":\"%s\",\"audited_namespace\":\"%s\",\"current_hits\":%llu,\"compatible_hits\":%llu,\"misses\":%llu}\n",
        vk_shader_cache_namespace(),ns,(unsigned long long)hits.current_hits,(unsigned long long)hits.compatible_hits,(unsigned long long)hits.misses);
    return ok?0:1;
}
