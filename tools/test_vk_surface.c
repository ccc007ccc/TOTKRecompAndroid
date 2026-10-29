// Exercise the real storage-image allocator/view upgrade and production-emitted
// SULD/SUST/SUATOM on Vulkan. Reuse the small Vulkan test's module/memory helpers.
#ifdef __ANDROID__
#define VK_NO_PROTOTYPES
#include <volk.h>
#include <stdarg.h>
#endif
#define main unused_multisample_main
#include "test_vk_multisample.c"
#undef main
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/nvn/renderer.h"

VkG g_vk;
uint64_t vk_collect_dead(void);
static VkCommandBuffer command;
static VkDebugUtilsMessengerEXT debug_messenger;
static uint64_t completed;
uint64_t prof_up_ns=~0ull,prof_up_cum,prof_views_created;
void rt_fatal_trace(void){abort();}
#ifdef __ANDROID__
void rt_fatal_message(const char *fmt,...){va_list args;va_start(args,fmt);vfprintf(stderr,fmt,args);va_end(args);abort();}
void vk_async_drain(void){}
#endif
void vk_device_fault_report(void){abort();}
VkCommandBuffer vk_cmd(void){return command;}
VkCommandBuffer vk_upload_cmd(void){return command;}
void vk_end_pass(void){}
uint64_t vk_pending_seq(void){return completed+1;}
uint64_t vk_completed_seq(void){return completed;}
uint64_t vk_frame_counter(void){return completed;}
void vk_rp_collect(uint64_t seq){(void)seq;}
void vk_rp_forget_view(VkImageView view){(void)view;}
void vk_alias_texture_destroyed(NvnTexture *t){(void)t;}
void vk_dl_note_upload(void *image,uint32_t w,uint32_t h,uint32_t level){(void)image;(void)w;(void)h;(void)level;}
int vk_pages_changed(uint64_t address,uint64_t size,uint32_t since,uint32_t *now){(void)address;(void)size;(void)since;(void)now;abort();}
int texpatch_identify(const NvnTexture *t){(void)t;return -1;}
uint32_t texpatch_generation(void){return 0;}
void texpatch_apply(int id,uint32_t level,uint8_t *data,uint32_t w,uint32_t h,uint32_t row){(void)id;(void)level;(void)data;(void)w;(void)h;(void)row;abort();}
uint64_t nvn_linear_rt_pitch(uint64_t bytes){(void)bytes;abort();}
void astc_decode_ldr(const uint8_t *src,uint32_t w,uint32_t h,uint32_t bw,uint32_t bh,bool srgb,uint8_t *dst){
    (void)src;(void)w;(void)h;(void)bw;(void)bh;(void)srgb;(void)dst;abort();
}
VkSpan vk_transient(uint64_t size,uint64_t align){(void)size;(void)align;abort();}
VkSpan vk_transient_copy(const void *data,uint64_t size,uint64_t align){(void)data;(void)size;(void)align;abort();}
void vk_full_barrier(VkCommandBuffer cb){
    VkMemoryBarrier2 barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&barrier};vkCmdPipelineBarrier2(cb,&dep);
}
static void begin(void){CHECK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&b));}
static void finish(void){
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(g_vk.queue,1,&s,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(g_vk.queue));++completed;vk_collect_dead();
}
static void initialize(void){
#ifdef __ANDROID__
    CHECK(volkInitialize());
#endif
    const char *layer="VK_LAYER_KHRONOS_validation",*ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledLayerCount=1,
        .ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&ext};
    bool validation=vkCreateInstance(&ii,NULL,&instance)==VK_SUCCESS;
    if(!validation){ii.enabledLayerCount=ii.enabledExtensionCount=0;CHECK(vkCreateInstance(&ii,NULL,&instance));}
#ifdef __ANDROID__
    volkLoadInstance(instance);
#endif
    if(validation){VkDebugUtilsMessengerCreateInfoEXT d={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=report};
        CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"))(instance,&d,NULL,&debug_messenger));}
    uint32_t n=8;VkPhysicalDevice candidates[8];CHECK(vkEnumeratePhysicalDevices(instance,&n,candidates));assert(n);gpu=candidates[0];
    VkPhysicalDeviceFeatures have;vkGetPhysicalDeviceFeatures(gpu,&have);
    assert(have.shaderStorageImageWriteWithoutFormat);VkPhysicalDeviceFeatures enabled={.shaderStorageImageWriteWithoutFormat=VK_TRUE,
        .shaderStorageImageExtendedFormats=have.shaderStorageImageExtendedFormats};
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=VK_TRUE};
    uint32_t family_count=32;VkQueueFamilyProperties families[32];vkGetPhysicalDeviceQueueFamilyProperties(gpu,&family_count,families);
    unsigned family=0;while(family<family_count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;assert(family<family_count);
    float priority=1;VkDeviceQueueCreateInfo q={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo d={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.pEnabledFeatures=&enabled,.queueCreateInfoCount=1,.pQueueCreateInfos=&q};
    CHECK(vkCreateDevice(gpu,&d,NULL,&device));g_vk.inst=instance;g_vk.phys=gpu;g_vk.dev=device;g_vk.qfam=family;
#ifdef __ANDROID__
    volkLoadDevice(device);
#endif
    vkGetPhysicalDeviceProperties(gpu,&g_vk.props);vkGetDeviceQueue(device,family,0,&g_vk.queue);
    printf("GPU: %s validation=%d\n",g_vk.props.deviceName,validation);
    VmaVulkanFunctions functions={.vkGetInstanceProcAddr=vkGetInstanceProcAddr,.vkGetDeviceProcAddr=vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo ai={.physicalDevice=gpu,.device=device,.instance=instance,.vulkanApiVersion=VK_API_VERSION_1_3,
        .pVulkanFunctions=&functions};CHECK(vmaCreateAllocator(&ai,&g_vk.vma));
    VkCommandPoolCreateInfo p={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CHECK(vkCreateCommandPool(device,&p,NULL,&g_vk.pool));VkCommandBufferAllocateInfo c={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=g_vk.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};CHECK(vkAllocateCommandBuffers(device,&c,&command));
}
static NvnTexture texture(unsigned dim,uint32_t format,uint32_t w,uint32_t h){
    NvnTexture t={.target=dim?1:0,.format=format,.width=w,.height=dim?h:1,.depth=1,.levels=1,.samples=1};
    t.layout=(TexLayout){.width=w,.height=t.height,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(format)};return t;
}
static void copy_subresource(NvnTexture *t,VkBuffer buffer,unsigned level,unsigned layer){
    TexBackend*b=t->backend;vk_full_barrier(command);
    uint32_t w=b->width>>level,h=b->height>>level;if(!w)w=1;if(!h)h=1;
    VkBufferImageCopy copy={.bufferOffset=4096,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.imageExtent={w,h,1}};
    vkCmdCopyImageToBuffer(command,b->image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
}
static void copy_pixels(NvnTexture *t,VkBuffer buffer){copy_subresource(t,buffer,0,0);}
int main(int argc,char**argv){
    assert(argc==2);initialize();VkBuffer buffer;
    VkBufferCreateInfo b={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=65536,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(device,&b,NULL,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
    VkDeviceMemory mem=memory(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);CHECK(vkBindBufferMemory(device,buffer,mem,0));
    uint32_t *out;CHECK(vkMapMemory(device,mem,0,b.size,0,(void**)&out));
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
                                            {32,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};VkDescriptorSetLayout layout;CHECK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    VkPipelineLayoutCreateInfo pi={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};VkPipelineLayout pl;CHECK(vkCreatePipelineLayout(device,&pi,NULL,&pl));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&pool_info,NULL,&pool));
    VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&alloc,&set));
    unsigned checks=0;
    for(unsigned dim=0;dim<2;++dim)for(unsigned op=0;op<6;++op){
        unsigned shader_op=op==5?2:op;uint32_t format=op<2?0x0b:op==2?0x2e:op==3?0x30:op==4?0x2f:0x25;
        NvnTexture t=texture(dim,format,4,2);begin();TexBackend *old=vk_tex(&t,true);
        VkClearColorValue seed={.uint32={0xfffffffeu,0xfffffffeu,0xfffffffeu,0xfffffffeu}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vk_full_barrier(command);vkCmdClearColorImage(command,old->image,VK_IMAGE_LAYOUT_GENERAL,&seed,1,&range);finish();
        begin();ShaderTexture shader={.is_image=true,.type=dim?TEXTYPE_2D:TEXTYPE_1D,.image_raw=op<2,.image_write=op!=0,
            .image_kind=op==0?2:op==1?1:shader_op-2};
        VkDescriptorImageInfo image_info={VK_NULL_HANDLE,vk_storage_view(&t,NULL,false,&shader),VK_IMAGE_LAYOUT_GENERAL};
        assert(((TexBackend*)t.backend)->storage);VkDescriptorBufferInfo buffer_info={buffer,0,4096};
        VkWriteDescriptorSet writes[]={
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_info},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=32,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&image_info}};
        vkUpdateDescriptorSets(device,2,writes,0,NULL);
        char path[1024];snprintf(path,sizeof path,"%s/surface-%u-%u.comp.spv",argv[1],dim,shader_op);VkPipeline p=pipeline(path,pl);
        vk_full_barrier(command);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,p);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,NULL);
        vkCmdDispatch(command,4,dim?2:1,1);copy_pixels(&t,buffer);finish();
        unsigned pixels=dim?8:4;
        for(unsigned i=0;i<pixels;++i){
            if(op<2){assert(out[i]==0xfffffffeu);assert(out[1024+i]==(op?1u:0xfffffffeu));checks+=2;}
            else if(op==5){const uint8_t expected[]={64,128,191,255};for(unsigned c=0;c<4;++c){assert(((uint8_t*)(out+1024))[4*i+c]==expected[c]);++checks;}}
            else {const uint32_t ints[]={0xfffffffcu,2,4,8};const float floats[]={0.25f,0.5f,0.75f,1};
                for(unsigned c=0;c<4;++c){if(op==2)assert(((float*)(out+1024))[4*i+c]==floats[c]);else assert(out[1024+4*i+c]==ints[c]);++checks;}}
        }
        vkDestroyPipeline(device,p,NULL);backend_texture_destroyed(&t);begin();finish();
    }
    // The first image binding may upgrade an existing scaled render target.
    // Preserve its contents while returning to guest texel dimensions.
    renderer_set_scene_scale(2,2);NvnTexture scaled=texture(1,0x0b,64,36);scaled.flags=8;
    begin();TexBackend *original=vk_tex(&scaled,true);assert(original->width==128 && original->height==72);
    VkClearColorValue seven={.uint32={7,7,7,7}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vk_full_barrier(command);
    vkCmdClearColorImage(command,original->image,VK_IMAGE_LAYOUT_GENERAL,&seven,1,&range);finish();begin();
    ShaderTexture raw={.is_image=true,.type=TEXTYPE_2D,.image_raw=true,.image_kind=2};vk_storage_view(&scaled,NULL,false,&raw);
    assert(((TexBackend*)scaled.backend)->width==64 && ((TexBackend*)scaled.backend)->height==36 && !renderer_texture_scalable(&scaled));
    copy_pixels(&scaled,buffer);finish();for(unsigned i=0;i<64*36;++i){assert(out[1024+i]==7);++checks;}
    backend_texture_destroyed(&scaled);begin();finish();
    // Below 1x the old allocation has fewer mips than the guest-sized image.
    // Check every retained mip/layer independently and the newly allocated tail.
    const float scales[]={0.5f,1,2};
    for(unsigned scale=0;scale<3;++scale){
        renderer_set_scene_scale(scales[scale],scales[scale]);NvnTexture mip=texture(1,0x0b,64,36);
        mip.flags=8;mip.target=4;mip.depth=mip.layout.layers=2;mip.levels=mip.layout.levels=7;
        begin();TexBackend *old=vk_tex(&mip,true);unsigned old_levels=old->levels;
        assert(old->width==(unsigned)(64*scales[scale]) && old_levels==(scale?7:6));
        vk_full_barrier(command);
        for(unsigned lv=0;lv<old_levels;++lv)for(unsigned layer=0;layer<2;++layer){
            VkClearColorValue seed={.uint32={100+3*lv+layer,0,0,0}};
            VkImageSubresourceRange sub={VK_IMAGE_ASPECT_COLOR_BIT,lv,1,layer,1};
            vkCmdClearColorImage(command,old->image,VK_IMAGE_LAYOUT_GENERAL,&seed,1,&sub);
        }
        finish();begin();
        NvnTextureView tail={.setmask=3,.base_level=6,.num_levels=1,.min_layer=1,.num_layers=1};
        assert(vk_storage_view(&mip,&tail,true,&raw));
        TexBackend *upgraded=mip.backend;assert(upgraded->width==64 && upgraded->height==36 && upgraded->levels==7 && upgraded->layers==2);
        finish();
        for(unsigned lv=0;lv<7;++lv)for(unsigned layer=0;layer<2;++layer){
            begin();copy_subresource(&mip,buffer,lv,layer);finish();
            unsigned w=64>>lv,h=36>>lv;if(!w)w=1;if(!h)h=1;
            uint32_t expected=lv<old_levels?100+3*lv+layer:0;
            for(unsigned pixel=0;pixel<w*h;++pixel){assert(out[1024+pixel]==expected);++checks;}
        }
        backend_texture_destroyed(&mip);begin();finish();
    }
    vkDestroyDescriptorPool(device,pool,NULL);vkDestroyPipelineLayout(device,pl,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);
    vkUnmapMemory(device,mem);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,mem,NULL);vkDestroyCommandPool(device,g_vk.pool,NULL);
    vmaDestroyAllocator(g_vk.vma);vkDestroyDevice(device,NULL);
    if(debug_messenger)((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,debug_messenger,NULL);
    vkDestroyInstance(instance,NULL);
    printf("PASS: %u storage-image values, real view upgrades, %u validation messages\n",checks,messages);return messages!=0;
}
