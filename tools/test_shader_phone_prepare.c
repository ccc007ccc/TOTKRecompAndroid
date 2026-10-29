// Android measurement adapter for the unmodified libraries from the built APK.
// Reads an existing RomFS through a symlink; all outputs stay in a separate root.
// No game execution, installation, or GPU arithmetic test.
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/settings.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static FILE *output;
static double started;
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void *symbol(void *library,const char *name){void *p=dlsym(library,name);if(!p){fprintf(output,"missing symbol %s: %s\n",name,dlerror());exit(2);}return p;}
static void progress(double value,const char *message){
    static double last=-100;double now=seconds();
    if(now-last>=5 || value==1){fprintf(output,"%.1fs %.3f%% %s\n",now-started,value*100,message);fflush(output);last=now;}
}
static unsigned long long files,base_files,logical_bytes,allocated_bytes,base_bytes,partial_files;
static unsigned long long reused_files,reused_bytes,reused_allocated,link_bytes,directory_bytes;
static void measure(const char *path){
    DIR *dir=opendir(path);if(!dir){perror(path);exit(2);}struct dirent *entry;
    struct stat directory;if(fstat(dirfd(dir),&directory)){perror(path);exit(2);}
    directory_bytes+=(unsigned long long)directory.st_blocks*512;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char child[PATH_MAX];if(snprintf(child,sizeof child,"%s/%s",path,entry->d_name)>=(int)sizeof child)exit(2);
        struct stat st;if(lstat(child,&st))exit(2);
        if(S_ISDIR(st.st_mode)){measure(child);continue;}
        if(S_ISLNK(st.st_mode)){
            link_bytes+=(unsigned long long)st.st_blocks*512;
            if(stat(child,&st) || !S_ISREG(st.st_mode)){
                fprintf(output,"invalid cache file link: %s\n",child);exit(2);
            }
            ++reused_files;reused_bytes+=st.st_size;reused_allocated+=(unsigned long long)st.st_blocks*512;
        }
        if(!S_ISREG(st.st_mode))continue;
        ++files;logical_bytes+=st.st_size;allocated_bytes+=(unsigned long long)st.st_blocks*512;
        if(strstr(entry->d_name,".base.bin")){++base_files;base_bytes+=st.st_size;}
        if(strstr(entry->d_name,".part"))++partial_files;
    }
    closedir(dir);
}
static void measurement(FILE *report){
    // Total data includes linked files; owned allocation excludes their targets.
    // Link and directory allocation are reported separately, without duplicating
    // the old cache's data in the incremental disk-space measurement.
    fprintf(report,"\"files\":%llu,\"base_files\":%llu,\"logical_bytes\":%llu,\"allocated_file_bytes\":%llu,\"base_bytes\":%llu,\"partial_files\":%llu,\"reused_files\":%llu,\"reused_logical_bytes\":%llu,\"reused_allocated_file_bytes\":%llu,\"owned_allocated_file_bytes\":%llu,\"link_metadata_bytes\":%llu,\"cache_directory_bytes\":%llu",
        files,base_files,logical_bytes,allocated_bytes,base_bytes,partial_files,reused_files,reused_bytes,reused_allocated,allocated_bytes-reused_allocated,link_bytes,directory_bytes);
}
int main(int argc,char **argv){
    output=fdopen(dup(STDOUT_FILENO),"w");setvbuf(output,NULL,_IOLBF,0);
    if(argc==3 && !strcmp(argv[1],"--measure")){
        measure(argv[2]);fputc('{',output);measurement(output);fputs("}\n",output);return 0;
    }
    if(argc!=4){fprintf(output,"usage: test_shader_phone_prepare ABS_LIBDIR ABS_ROMFS ABS_OUTPUT\n");return 2;}
    char libraries[PATH_MAX],romfs[PATH_MAX],root[PATH_MAX],path[PATH_MAX];
    if(!realpath(argv[1],libraries) || !realpath(argv[2],romfs))return 2;
    if(mkdir(argv[3],0755) && errno!=EEXIST)return 2;
    if(!realpath(argv[3],root) || chdir(root))return 2;
    int lock=open("measurement.lock",O_WRONLY|O_CREAT,0644);if(lock<0 || flock(lock,LOCK_EX|LOCK_NB))return 2;
    if(mkdir("game",0755) && errno!=EEXIST)return 2;
    if(symlink(romfs,"game/romfs.bin")){
        char old[PATH_MAX];ssize_t n=readlink("game/romfs.bin",old,sizeof old-1);if(n<0)return 2;old[n]=0;if(strcmp(old,romfs))return 2;
    }
    setenv("LARECOMP_NATIVE_LIB_DIR",libraries,1);
    snprintf(path,sizeof path,"%s/runtime.log",root);setenv("LARECOMP_LOG_FILE",path,1);
    // Resolve system loader dependencies before opening libmain in this shell
    // namespace: adrenotools also exports android_get_exported_namespace as data.
    // App launches use a separate class-loader namespace instead.
    if(getenv("LARECOMP_VK_DRIVER")){fprintf(output,"this measurement uses the system Vulkan driver\n");return 2;}
    VkG measured={0};VkG *gpu=&measured;
    void *vulkan=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);if(!vulkan)return 2;
    PFN_vkGetInstanceProcAddr get_proc=(PFN_vkGetInstanceProcAddr)symbol(vulkan,"vkGetInstanceProcAddr");
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get_proc(VK_NULL_HANDLE,"vkCreateInstance");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    if(create(&ci,NULL,&gpu->inst)!=VK_SUCCESS)return 2;
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get_proc(gpu->inst,"vkEnumeratePhysicalDevices");
    uint32_t count=1;if(enumerate(gpu->inst,&count,&gpu->phys)<0 || !count)return 2;
    PFN_vkGetPhysicalDeviceProperties props=(PFN_vkGetPhysicalDeviceProperties)get_proc(gpu->inst,"vkGetPhysicalDeviceProperties");
    props(gpu->phys,&gpu->props);gpu->api=VK_API_VERSION_1_3;
    PFN_vkGetPhysicalDeviceFeatures features=(PFN_vkGetPhysicalDeviceFeatures)get_proc(gpu->inst,"vkGetPhysicalDeviceFeatures");
    VkPhysicalDeviceFeatures available;features(gpu->phys,&available);gpu->shader_int64=available.shaderInt64;
    gpu->image_write_without_format=available.shaderStorageImageWriteWithoutFormat;
    snprintf(path,sizeof path,"%s/libmain.so",libraries);
    void *runtime=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!runtime){fprintf(output,"load runtime: %s\n",dlerror());return 2;}
    gpu=symbol(runtime,"g_vk");*gpu=measured;
    Settings *settings=symbol(runtime,"g_settings");
    ((void (*)(void))symbol(runtime,"settings_load"))();settings->shader_fp32=true;
    ((void (*)(void))symbol(runtime,"vk_android_load"))();
    ((void (*)(VkInstance))symbol(runtime,"volkLoadInstance"))(gpu->inst);
    bool qcom=((bool (*)(void))symbol(runtime,"vk_driver_is_qualcomm"))();
    unsigned long long guest=*(uint64_t*)symbol(runtime,"g_guest_lo");
    fprintf(output,"device=%s vendor=%u driver=%u api=%u qualcomm=%d shaderInt64=%d fp32=%d int32_setting=%d guest=%llx\n",
        gpu->props.deviceName,gpu->props.vendorID,gpu->props.driverVersion,gpu->api,qcom,gpu->shader_int64,settings->shader_fp32,settings->shader_int32,guest);
    started=seconds();
    ((void (*)(void))symbol(runtime,"vk_shaders_init"))();
    bool (*prepare)(const char*,void (*)(double,const char*),char*,size_t)=symbol(runtime,"vk_shader_prepare_game");
    bool (*ready)(const char*)=symbol(runtime,"vk_shader_game_ready");
    char why[8192]={0};bool ok=prepare("game",progress,why,sizeof why);double elapsed=seconds()-started;
    bool complete=ready("game");measure("cache");struct rusage usage;getrusage(RUSAGE_SELF,&usage);
    FILE *report=fopen("measurement.json","w");if(!report)return 2;
    fprintf(report,"{\"prepared\":%s,\"ready\":%s,\"seconds\":%.3f,",ok?"true":"false",complete?"true":"false",elapsed);
    measurement(report);
    fprintf(report,",\"peak_rss_kib\":%ld,\"guest_base\":%llu,\"vulkan_api\":%u,\"qualcomm\":%s,\"fp32\":%s,\"gameplay_tested\":false}\n",
        usage.ru_maxrss,guest,gpu->api,qcom?"true":"false",settings->shader_fp32?"true":"false");
    fclose(report);fprintf(output,"RESULT prepared=%d ready=%d seconds=%.3f files=%llu logical=%llu allocated=%llu peak_rss_kib=%ld error=%s\n",ok,complete,elapsed,files,logical_bytes,allocated_bytes,usage.ru_maxrss,why);
    return ok && complete?0:1;
}
