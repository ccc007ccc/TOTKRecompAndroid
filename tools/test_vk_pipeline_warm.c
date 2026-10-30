// Persist recipes, reload them in a new process, warm using the production
// pipeline builder, then render with a separately recreated compatible layout.
#include "../runtime/nvn/vulkan/pipeline_warm.h"
#include "../runtime/nvn/vulkan/pipeline_seed.h"
#include "../runtime/nvn/vulkan/shader_module_cache.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do {VkResult r_=(x);if(r_!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#x,r_);exit(1);}} while(0)
VkG g_vk;
void rt_fatal_trace(void){abort();}
void vk_device_fault_report(void){}
#ifdef __ANDROID__
void rt_fatal_message(const char *format,...){va_list a;va_start(a,format);vfprintf(stderr,format,a);va_end(a);abort();}
#endif
const char *vk_shader_cache_namespace(void){return "pipeline-gpu-test";}
static unsigned messages;
static const char *vertex_file,*fragment_file,*compute_file;
static const VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE,VK_DYNAMIC_STATE_FRONT_FACE,VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,VK_DYNAMIC_STATE_STENCIL_OP,
    VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;if(severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){++messages;fprintf(stderr,"%s\n",data->pMessage);}return VK_FALSE;
}
static VkDeviceMemory memory(VkMemoryRequirements r,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(g_vk.phys,&p);
    for(unsigned i=0;i<p.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) {
        VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=i};
        VkDeviceMemory m;CHECK(vkAllocateMemory(g_vk.dev,&a,NULL,&m));return m;
    }
    abort();
}
static VkShaderModule module(const char *name) {
    FILE *f=fopen(name,"rb");assert(f && !fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>20 && !(bytes%4));rewind(f);
    uint32_t *words=malloc(bytes);assert(words && fread(words,1,bytes,f)==(size_t)bytes && !fclose(f));
    VkShaderModuleCreateInfo i={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=words};
    VkShaderModule m;CHECK(vkCreateShaderModule(g_vk.dev,&i,NULL,&m));free(words);return m;
}
#ifdef TOTK_TEST_MODULE_REUSE
uint32_t *vk_shader_cache_load_key(uint64_t key,bool base,size_t *n) {
    assert(!base);const char *name=key==1?vertex_file:key==2?fragment_file:compute_file;
    FILE *f=fopen(name,"rb");assert(f && !fseek(f,0,SEEK_END));long size=ftell(f);assert(size>20 && !(size%4));rewind(f);
    uint32_t *words=malloc(size);assert(words && fread(words,1,size,f)==(size_t)size && !fclose(f));*n=size/4;return words;
}
#endif
static VkPipelineLayout layout(VkDescriptorSetLayout sets[2]) {
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    for(unsigned i=0;i<2;++i){VkDescriptorSetLayoutCreateInfo d={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=i,.pBindings=&binding};CHECK(vkCreateDescriptorSetLayout(g_vk.dev,&d,NULL,&sets[i]));}
    VkPipelineLayoutCreateInfo c={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=sets};
    VkPipelineLayout p;CHECK(vkCreatePipelineLayout(g_vk.dev,&c,NULL,&p));return p;
}
static VkPipelineLayout compute_layout(VkDescriptorSetLayout *set) {
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo d={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};CHECK(vkCreateDescriptorSetLayout(g_vk.dev,&d,NULL,set));
    VkPipelineLayoutCreateInfo l={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=set};
    VkPipelineLayout p;CHECK(vkCreatePipelineLayout(g_vk.dev,&l,NULL,&p));return p;
}
static VkResult build(const VkPipelineRecipe *r,VkPipelineWarmProduct *product) {
    if(!r->shader[1]) {
#ifdef TOTK_TEST_MODULE_REUSE
        VkShaderModuleLease lease;assert(vk_shader_module_acquire(r->shader[0],5,NULL,0,&lease));
        assert(lease.count==1 && lease.bindings[0].type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        VkShaderModule cs=lease.module;
#else
        VkShaderModule cs=module(compute_file);
#endif
        VkDescriptorSetLayout set;VkPipelineLayout p=compute_layout(&set);
        VkResult result=vk_pipeline_compute_build(cs,p,r->local,&product->pipeline);
        product->layout=p;product->sets[0]=set;
#ifdef TOTK_TEST_MODULE_REUSE
        vk_shader_module_release(&lease);
#else
        vkDestroyShaderModule(g_vk.dev,cs,NULL);
#endif
        return result;
    }
#ifdef TOTK_TEST_MODULE_REUSE
    VkShaderModuleLease leases[2];assert(vk_shader_module_acquire(r->shader[0],0,NULL,0,&leases[0]));assert(vk_shader_module_acquire(r->shader[1],1,NULL,0,&leases[1]));
    assert(leases[0].count==0 && leases[1].count==1 && leases[1].bindings[0].set==1 && leases[1].bindings[0].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    VkShaderModule vs=leases[0].module,fs=leases[1].module;
#else
    VkShaderModule vs=module(vertex_file),fs=module(fragment_file);
#endif
    VkDescriptorSetLayout sets[2];VkPipelineLayout p=layout(sets);
    VkResult result=vk_pipeline_state_build(&r->state,vs,fs,p,g_vk.eds?17:7,dynamic,0,&product->pipeline);
    product->layout=p;memcpy(product->sets,sets,sizeof sets);
#ifdef TOTK_TEST_MODULE_REUSE
    for(unsigned i=0;i<2;++i)vk_shader_module_release(&leases[i]);
#else
    vkDestroyShaderModule(g_vk.dev,vs,NULL);vkDestroyShaderModule(g_vk.dev,fs,NULL);
#endif
    return result;
}
int main(int argc,char **argv) {
    assert(argc==6);bool write_only=!strcmp(argv[1],"--record"),seed_only=!strcmp(argv[1],"--render-seed");vertex_file=argv[3];fragment_file=argv[4];compute_file=argv[5];
#ifdef __ANDROID__
    CHECK(volkInitialize());
#endif
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    const char *layer="VK_LAYER_KHRONOS_validation",*ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&ext};
    bool validation=vkCreateInstance(&ii,NULL,&g_vk.inst)==VK_SUCCESS;
    if(!validation){ii.enabledLayerCount=ii.enabledExtensionCount=0;CHECK(vkCreateInstance(&ii,NULL,&g_vk.inst));}
#ifdef __ANDROID__
    volkLoadInstance(g_vk.inst);
#endif
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    if(validation){VkDebugUtilsMessengerCreateInfoEXT d={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=report};
        CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkCreateDebugUtilsMessengerEXT"))(g_vk.inst,&d,NULL,&messenger));}
    uint32_t n=8;VkPhysicalDevice devices[8];CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&n,devices));assert(n);g_vk.phys=devices[0];
    vkGetPhysicalDeviceProperties(g_vk.phys,&g_vk.props);printf("GPU %s; validation=%d\n",g_vk.props.deviceName,validation);
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,.pNext=&eds2};
    VkPhysicalDeviceDynamicRenderingFeatures rendering={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,.pNext=&eds};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&rendering};vkGetPhysicalDeviceFeatures2(g_vk.phys,&features);
    assert(rendering.dynamicRendering);
    eds2.extendedDynamicState2LogicOp=VK_FALSE;eds2.extendedDynamicState2PatchControlPoints=VK_FALSE;
    VkQueueFamilyProperties families[32];n=32;vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&n,families);
    while(g_vk.qfam<n && !(families[g_vk.qfam].queueFlags&VK_QUEUE_GRAPHICS_BIT))++g_vk.qfam;assert(g_vk.qfam<n);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&rendering,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};CHECK(vkCreateDevice(g_vk.phys,&di,NULL,&g_vk.dev));
#ifdef __ANDROID__
    volkLoadDevice(g_vk.dev);
#endif
    vkGetDeviceQueue(g_vk.dev,g_vk.qfam,0,&g_vk.queue);VkPipelineCacheCreateInfo pci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};CHECK(vkCreatePipelineCache(g_vk.dev,&pci,NULL,&g_vk.pcache));
    unsigned checked=0,modes=eds.extendedDynamicState && eds2.extendedDynamicState2?4:2;
    VkRenderPass retained_pass=VK_NULL_HANDLE;
    for(unsigned mode=0;mode<modes;++mode) {
        g_vk.dyn_render=mode&1;g_vk.eds=g_vk.eds2=mode>=2;
        char directory[1400];snprintf(directory,sizeof directory,"%s/%s%s",argv[2],(mode&1)?"dynamic":"classic",mode>=2?"-eds":"");
        mkdir(directory,0755);assert(!chdir(directory));
        VkPipelineState state={0};state.ncolor=1;state.color[0]=VK_FORMAT_R32G32B32A32_SFLOAT;state.samples=1;
        state.topo_class=2;state.poly_fill=1;state.write_mask[0]=15;state.fx.on=!g_vk.eds;
        if(state.fx.on)state.fx.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRecipe recipe;vk_pipeline_recipe_make(&recipe,1,2,&state);
        uint32_t local[3]={2,3,4};VkPipelineRecipe compute;vk_pipeline_recipe_compute(&compute,5,local);
        if(seed_only){remove("cache/vk_pipeline_recipes.bin");vk_pipeline_warm_start_seed(build,"seed.bin",143);assert(vk_pipeline_warm_stats().seeded==2);}
        else vk_pipeline_warm_start(write_only?NULL:build);
        if(write_only){vk_pipeline_warm_record(&recipe);vk_pipeline_warm_record(&compute);assert(vk_pipeline_warm_save());VkPipelineRecipe seeds[]={recipe,compute};VkPipelineSeedIdentity id=vk_pipeline_seed_identity(143);assert(vk_pipeline_seed_save("seed.bin",&id,seeds,2));vk_pipeline_warm_stop();continue;}
        for(unsigned i=0;i<10000 && vk_pipeline_warm_stats().prepared<2;++i)usleep(1000);
        assert(vk_pipeline_warm_stats().prepared==2);VkPipelineWarmProduct graphics_product,compute_product;
        assert(vk_pipeline_warm_take(&recipe,&graphics_product));assert(vk_pipeline_warm_take(&compute,&compute_product));
        VkPipeline pipeline=graphics_product.pipeline,compute_pipeline=compute_product.pipeline;
        vk_pipeline_warm_stop(); // the taken pipeline must remain valid
        VkDescriptorSetLayout sets[2];VkPipelineLayout p=layout(sets);
        VkDescriptorSetLayout cs_set;VkPipelineLayout cs_layout=compute_layout(&cs_set);
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=512,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VkBuffer buffer;CHECK(vkCreateBuffer(g_vk.dev,&bi,NULL,&buffer));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(g_vk.dev,buffer,&requirements);
        VkDeviceMemory bm=memory(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);CHECK(vkBindBufferMemory(g_vk.dev,buffer,bm,0));
        float *data;CHECK(vkMapMemory(g_vk.dev,bm,0,512,0,(void **)&data));data[0]=2;data[1]=3;data[2]=5;data[3]=7;
        VkDescriptorPoolSize poolsize[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=2,.pPoolSizes=poolsize};
        VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(g_vk.dev,&dpi,NULL,&pool));VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&sets[1]};
        VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(g_vk.dev,&dai,&set));VkDescriptorBufferInfo dbi={buffer,0,16};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&dbi};vkUpdateDescriptorSets(g_vk.dev,1,&write,0,NULL);
        dai.pSetLayouts=&cs_set;VkDescriptorSet compute_set;CHECK(vkAllocateDescriptorSets(g_vk.dev,&dai,&compute_set));
        dbi.range=96;write.dstSet=compute_set;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;vkUpdateDescriptorSets(g_vk.dev,1,&write,0,NULL);
        VkImageCreateInfo ici={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=state.color[0],.extent={4,4,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage image;CHECK(vkCreateImage(g_vk.dev,&ici,NULL,&image));vkGetImageMemoryRequirements(g_vk.dev,image,&requirements);VkDeviceMemory im=memory(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);CHECK(vkBindImageMemory(g_vk.dev,image,im,0));
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageViewCreateInfo vci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=state.color[0],.subresourceRange=range};VkImageView view;CHECK(vkCreateImageView(g_vk.dev,&vci,NULL,&view));
        VkRenderPass pass=vk_rp_compat(1,state.color,0,0,VK_SAMPLE_COUNT_1_BIT);VkFramebuffer framebuffer=VK_NULL_HANDLE;
        if(pass)retained_pass=pass;
        if(pass){VkFramebufferCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,.pAttachments=&view,.width=4,.height=4,.layers=1};CHECK(vkCreateFramebuffer(g_vk.dev,&fci,NULL,&framebuffer));}
        VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=g_vk.qfam};VkCommandPool commands;CHECK(vkCreateCommandPool(g_vk.dev,&cpi,NULL,&commands));VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer cb;CHECK(vkAllocateCommandBuffers(g_vk.dev,&cai,&cb));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cb,&begin));
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue clear={.float32={-99,-99,-99,-99}};vkCmdClearColorImage(cb,image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
        barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&barrier);
        if(pass){VkRenderPassBeginInfo r={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,.renderArea={{0,0},{4,4}}};vkCmdBeginRenderPass(cb,&r,VK_SUBPASS_CONTENTS_INLINE);}
        else {VkRenderingAttachmentInfo a={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};VkRenderingInfo r={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{4,4}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&a};vkCmdBeginRendering(cb,&r);}
        VkViewport viewport={0,0,4,4,0,1};VkRect2D scissor={{0,0},{4,4}};vkCmdSetViewport(cb,0,1,&viewport);vkCmdSetScissor(cb,0,1,&scissor);vkCmdSetDepthBias(cb,0,0,0);float blend[4]={0};vkCmdSetBlendConstants(cb,blend);vkCmdSetStencilCompareMask(cb,VK_STENCIL_FACE_FRONT_AND_BACK,255);vkCmdSetStencilWriteMask(cb,VK_STENCIL_FACE_FRONT_AND_BACK,255);vkCmdSetStencilReference(cb,VK_STENCIL_FACE_FRONT_AND_BACK,0);
        if(g_vk.eds){vkCmdSetCullMode(cb,VK_CULL_MODE_NONE);vkCmdSetFrontFace(cb,VK_FRONT_FACE_COUNTER_CLOCKWISE);vkCmdSetPrimitiveTopology(cb,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);vkCmdSetDepthTestEnable(cb,VK_FALSE);vkCmdSetDepthWriteEnable(cb,VK_FALSE);vkCmdSetDepthCompareOp(cb,VK_COMPARE_OP_ALWAYS);vkCmdSetStencilTestEnable(cb,VK_FALSE);vkCmdSetStencilOp(cb,VK_STENCIL_FACE_FRONT_AND_BACK,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS);vkCmdSetDepthBiasEnable(cb,VK_FALSE);vkCmdSetPrimitiveRestartEnable(cb,VK_FALSE);}
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);uint32_t offset=0;vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,p,1,1,&set,1,&offset);vkCmdDraw(cb,3,1,0,0);
        if(pass)vkCmdEndRenderPass(cb);else vkCmdEndRendering(cb);
        VkMemoryBarrier compute_before={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_UNIFORM_READ_BIT,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&compute_before,0,NULL,0,NULL);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,compute_pipeline);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,cs_layout,0,1,&compute_set,0,NULL);vkCmdDispatch(cb,1,1,1);
        barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy copy={.bufferOffset=256,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={4,4,1}};vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
        VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(cb));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};CHECK(vkQueueSubmit(g_vk.queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(g_vk.queue));
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const float expected[]={x+2.5f,y+3.5f,5,7};for(unsigned c=0;c<4;++c){assert(data[64+4*(x+4*y)+c]==expected[c]);++checked;}}
        for(unsigned i=0;i<24;++i){assert(((uint32_t*)data)[i]==31+(i%2)+10*((i/2)%3)+100*(i/6));++checked;}
        vk_pipeline_warm_discard(&compute_product);vkDestroyPipelineLayout(g_vk.dev,cs_layout,NULL);vkDestroyDescriptorSetLayout(g_vk.dev,cs_set,NULL);
        vkDestroyCommandPool(g_vk.dev,commands,NULL);if(framebuffer)vkDestroyFramebuffer(g_vk.dev,framebuffer,NULL);
        vkDestroyImageView(g_vk.dev,view,NULL);vkDestroyImage(g_vk.dev,image,NULL);vkFreeMemory(g_vk.dev,im,NULL);vkUnmapMemory(g_vk.dev,bm);vkDestroyBuffer(g_vk.dev,buffer,NULL);vkFreeMemory(g_vk.dev,bm,NULL);vkDestroyDescriptorPool(g_vk.dev,pool,NULL);vk_pipeline_warm_discard(&graphics_product);vkDestroyPipelineLayout(g_vk.dev,p,NULL);for(unsigned i=0;i<2;++i)vkDestroyDescriptorSetLayout(g_vk.dev,sets[i],NULL);
    }
    if(retained_pass)vkDestroyRenderPass(g_vk.dev,retained_pass,NULL);
#ifdef TOTK_TEST_MODULE_REUSE
    if(!write_only){VkShaderModuleCacheStats stats=vk_shader_module_cache_stats();assert(stats.builds==3 && stats.hits>=3*(modes-1));}
    vk_shader_module_cache_reset();
#endif
    vkDestroyPipelineCache(g_vk.dev,g_vk.pcache,NULL);vkDestroyDevice(g_vk.dev,NULL);
    if(messenger)((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkDestroyDebugUtilsMessengerEXT"))(g_vk.inst,messenger,NULL);
    vkDestroyInstance(g_vk.inst,NULL);
    if(write_only)printf("PASS: recipes persisted for a new process; validation messages=%u\n",messages);
    else printf("PASS: %u warmed graphics/compute GPU values, classic/dynamic rendering, EDS=%d, compatible replacement layout; validation messages=%u\n",checked,modes==4,messages);
    return messages?1:0;
}
