// Real Vulkan execution of deswizzle.c, with CPU tegra_deswizzle as the independent reference.
// The small allocator stands in for the transient ring; production conversion, descriptors and barriers run unchanged.
#define _POSIX_C_SOURCE 200809L
#define main unused_multisample_main
#include "test_vk_multisample.c"
#undef main
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/nvn/vulkan/deswizzle_shader.h"

VkG g_vk;
static VkCommandBuffer command;
static VkDescriptorPool descriptors;
static VkBuffer ring, readback;
static VkDeviceMemory ring_mem, read_mem;
static uint8_t *mapped, *result_bytes;
static uint64_t used;
static VkShaderModule shader;
static unsigned allocations, checks, fail_stage;
static int live_sets, live_layouts, live_pipes;
static const uint64_t capacity = 96ull << 20;
void vk_device_fault_report(void) { abort(); }
void rt_fatal_trace(void) { abort(); }

// Fail after each partial initialization step and count all surviving handles.
VkResult __real_vkCreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout*);
VkResult __wrap_vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorSetLayout *o) {
    if (fail_stage==1) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=__real_vkCreateDescriptorSetLayout(d,i,a,o); if (!r) ++live_sets; return r;
}
VkResult __real_vkCreatePipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout*);
VkResult __wrap_vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkPipelineLayout *o) {
    if (fail_stage==2) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=__real_vkCreatePipelineLayout(d,i,a,o); if (!r) ++live_layouts; return r;
}
VkResult __real_vkCreateComputePipelines(VkDevice,VkPipelineCache,uint32_t,const VkComputePipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*);
VkResult __wrap_vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *o) {
    if (fail_stage==3) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=__real_vkCreateComputePipelines(d,c,n,i,a,o); if (!r) live_pipes+=n; return r;
}
void __real_vkDestroyDescriptorSetLayout(VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*);
void __wrap_vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout o,const VkAllocationCallbacks *a) {
    --live_sets; __real_vkDestroyDescriptorSetLayout(d,o,a);
}
void __real_vkDestroyPipelineLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*);
void __wrap_vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout o,const VkAllocationCallbacks *a) {
    --live_layouts; __real_vkDestroyPipelineLayout(d,o,a);
}
void __real_vkDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*);
void __wrap_vkDestroyPipeline(VkDevice d,VkPipeline o,const VkAllocationCallbacks *a) {
    --live_pipes; __real_vkDestroyPipeline(d,o,a);
}
VkShaderModule vk_builtin_shader(const char *name) { assert(!strcmp(name,"deswizzle.comp")); return fail_stage==4 ? VK_NULL_HANDLE : shader; }
VkSpan vk_transient(uint64_t size,uint64_t align) {
    uint64_t offset=(used+align-1)&~(align-1); assert(offset+size+64<=capacity);
    memset(mapped+offset,0xa5,size+64); used=offset+size+64; ++allocations;
    return (VkSpan){ring,offset,mapped+offset};
}
VkSpan vk_transient_copy(const void *data,uint64_t size,uint64_t align) {
    VkSpan s=vk_transient(size,align); memcpy(s.ptr,data,size); return s;
}
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo a={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device,&a,&set)); return set;
}
static void buffer(VkBuffer *b,VkDeviceMemory *m,uint8_t **p) {
    VkBufferCreateInfo i={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=capacity,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(device,&i,NULL,b)); VkMemoryRequirements r; vkGetBufferMemoryRequirements(device,*b,&r);
    *m=memory(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device,*b,*m,0)); CHECK(vkMapMemory(device,*m,0,capacity,0,(void**)p));
}
static VkDebugUtilsMessengerEXT messenger;
static void initialize(const char *path) {
    const char *layer="VK_LAYER_KHRONOS_validation",*ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf={.sType=VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,.enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&sync};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledLayerCount=1,
        .ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&ext,.pNext=&vf};
    CHECK(vkCreateInstance(&ii,NULL,&instance));
    VkDebugUtilsMessengerCreateInfoEXT di={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=report};
    CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"))(instance,&di,NULL,&messenger));
    uint32_t n=8; VkPhysicalDevice candidates[8]; CHECK(vkEnumeratePhysicalDevices(instance,&n,candidates)); assert(n); gpu=candidates[0];
    uint32_t count=32; VkQueueFamilyProperties families[32]; vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families);
    unsigned family=0; while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family; assert(family<count);
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=VK_TRUE};
    VkDeviceCreateInfo d={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    CHECK(vkCreateDevice(gpu,&d,NULL,&device)); g_vk.dev=device;
    vkGetPhysicalDeviceProperties(gpu,&g_vk.props); vkGetDeviceQueue(device,family,0,&g_vk.queue);
    printf("GPU: %s (synchronization validation enabled)\n",g_vk.props.deviceName);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CHECK(vkCreateCommandPool(device,&pi,NULL,&g_vk.pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=g_vk.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(vkAllocateCommandBuffers(device,&ca,&command));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,64};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=32,.poolSizeCount=1,.pPoolSizes=&size};
    CHECK(vkCreateDescriptorPool(device,&dp,NULL,&descriptors));
    buffer(&ring,&ring_mem,&mapped); buffer(&readback,&read_mem,&result_bytes); shader=module(path);
}
static void begin(void) {
    CHECK(vkResetDescriptorPool(device,descriptors,0)); CHECK(vkResetCommandBuffer(command,0)); used=256;
    VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command,&b));
}
static void finish(void) {
    VkMemoryBarrier2 mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dep={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&mb}; vkCmdPipelineBarrier2(command,&dep);
    CHECK(vkEndCommandBuffer(command)); VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(g_vk.queue,1,&s,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(g_vk.queue));
}
static void rejected(void) {
    uint8_t src[512]={0}; VkSpan dst={ring,777,mapped}, before=dst; unsigned old=allocations;
    assert(!vk_deswizzle_upload(command,src,sizeof src,64,8,1,0,0,&dst));
    assert(dst.buf==before.buf && dst.off==before.off && dst.ptr==before.ptr && allocations==old);
}
static void run_level(const TexLayout *t,unsigned level,unsigned layer) {
    LevelLayout l=tegra_level(t,level); uint32_t row=l.wb*t->fmt->bpb,pc[8]; uint64_t bytes;
    assert(vk_deswizzle_params(row,l.hb,l.d,l.block_height_log2,l.block_depth_log2,0,pc,&bytes));
    uint64_t size=(uint64_t)row*l.hb*l.d;
    uint8_t *src=malloc(bytes),*expected=malloc(size); assert(src && expected);
    for(uint64_t i=0;i<bytes;++i) src[i]=(uint8_t)((i*73)^(i>>7)^(i>>15)^(level*37)^(layer*51));
    tegra_deswizzle(src,expected,l.wb,l.hb,l.d,t->fmt->bpb,l.block_height_log2,l.block_depth_log2);
    begin(); VkSpan out;
    assert(vk_deswizzle_upload(command,src,bytes,row,l.hb,l.d,l.block_height_log2,l.block_depth_log2,&out));
    // Changing and freeing the original bytes before submission must not change the uploaded version.
    memset(src,0,bytes); free(src);
    VkBufferCopy copy={out.off,0,size}; vkCmdCopyBuffer(command,out.buf,readback,1,&copy);
    finish();
    if(memcmp(result_bytes,expected,size)) { fprintf(stderr,"Mismatch %ux%ux%u bpb %u mip %u layer %u\n",t->width,t->height,t->depth,t->fmt->bpb,level,layer); abort(); }
    for(unsigned i=0;i<64;++i) assert(mapped[out.off+size+i]==0xa5);
    free(expected); ++checks;
}
int main(int argc,char **argv) {
    assert(argc==2); initialize(argv[1]);
    unsetenv("LARECOMP_VK_GPU_DETILE"); rejected(); vk_deswizzle_reset();
    setenv("LARECOMP_VK_GPU_DETILE","1",1);
    for(fail_stage=1;fail_stage<=4;++fail_stage) {
        rejected(); assert(!live_sets && !live_layouts && !live_pipes); vk_deswizzle_reset();
    }
    fail_stage=0;
    VkPhysicalDeviceLimits saved=g_vk.props.limits;
    g_vk.props.limits.maxStorageBufferRange=511; rejected(); g_vk.props.limits=saved;
    g_vk.props.limits.maxComputeWorkGroupSize[0]=63; rejected(); g_vk.props.limits=saved;
    g_vk.props.limits.maxComputeWorkGroupInvocations=63; rejected(); g_vk.props.limits=saved;
    g_vk.props.limits.maxComputeWorkGroupCount[0]=0; rejected(); g_vk.props.limits=saved;
    uint8_t src[512]={0}; VkSpan out;
    assert(!vk_deswizzle_upload(command,src,511,64,8,1,0,0,&out));
    assert(!vk_deswizzle_upload(command,NULL,512,64,8,1,0,0,&out));
    assert(!vk_deswizzle_upload(command,src,512,64,8,1,0,0,NULL));
    const FormatInfo formats[]={{.bw=1,.bh=1,.bpb=4},{.bw=4,.bh=4,.bpb=8},{.bw=4,.bh=4,.bpb=16},{.bw=5,.bh=4,.bpb=16},{.bw=12,.bh=12,.bpb=16}};
    for(unsigned fmt=0;fmt<5;++fmt) for(unsigned volume=0;volume<2;++volume) {
        TexLayout t={.width=160,.height=157,.depth=volume?17:1,.levels=8,.layers=volume?1:2,.fmt=&formats[fmt]};
        tegra_layout_init(&t);
        for(unsigned layer=0;layer<t.layers;++layer) for(unsigned level=0;level<t.levels;++level) run_level(&t,level,layer);
        assert(live_sets==1 && live_layouts==1 && live_pipes==1); vk_deswizzle_reset();
        assert(!live_sets && !live_layouts && !live_pipes);
    }
    // The observed 5120x5120 BC4 atlas exceeds a single dispatch's work count (grid-stride path).
    TexLayout atlas={.width=5120,.height=5120,.depth=1,.levels=7,.layers=1,.fmt=&formats[1]}; tegra_layout_init(&atlas);
    for(unsigned level=0;level<atlas.levels;++level) run_level(&atlas,level,0);
    // Exercise both tile dimensions and a device limiting dispatches to one workgroup.
    TexLayout both={.width=67,.height=65,.depth=9,.levels=1,.layers=1,.fmt=&formats[0],.block_height_log2=3,.block_depth_log2=3};
    g_vk.props.limits.maxComputeWorkGroupCount[0]=1; run_level(&both,0,0); g_vk.props.limits=saved;
    vk_deswizzle_reset(); vk_deswizzle_reset(); assert(!live_sets && !live_layouts && !live_pipes);
    vkDestroyShaderModule(device,shader,NULL); vkDestroyDescriptorPool(device,descriptors,NULL); vkDestroyCommandPool(device,g_vk.pool,NULL);
    vkUnmapMemory(device,ring_mem); vkUnmapMemory(device,read_mem);
    vkDestroyBuffer(device,ring,NULL); vkDestroyBuffer(device,readback,NULL); vkFreeMemory(device,ring_mem,NULL); vkFreeMemory(device,read_mem,NULL);
    vkDestroyDevice(device,NULL);
    ((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,NULL);
    vkDestroyInstance(instance,NULL); assert(!messages);
    printf("PASS: %u real GPU levels, source snapshots, transfer visibility, bounds, failure cleanup and reset\n",checks);
}
