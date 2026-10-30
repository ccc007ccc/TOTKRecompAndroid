// Rebuild actual prepared compute recipes on a host GPU. No dispatch, guest
// memory, phone access or writable game input is involved.
#include "../runtime/nvn/vulkan/pipeline_seed.h"
#include "../runtime/nvn/vulkan/shader_module_cache.h"
#include "../runtime/nvn/vulkan/compile_budget.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(call) do{VkResult result=(call);if(result!=VK_SUCCESS){fprintf(stderr,"%s = %d\n",#call,result);abort();}}while(0)
VkG g_vk;
void rt_fatal_trace(void){abort();}
static _Atomic unsigned messages;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;if(severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){++messages;fprintf(stderr,"%s\n",data->pMessage);}return VK_FALSE;
}
static VkResult build(const VkPipelineRecipe *recipe,VkPipelineWarmProduct *out) {
    assert(!recipe->shader[1]);VkShaderModuleLease lease;
    if(!vk_shader_module_acquire(recipe->shader[0],5,NULL,0,&lease))return VK_NOT_READY;
    VkResult result=vk_try_set_layout(lease.bindings,lease.count,0,VK_SHADER_STAGE_COMPUTE_BIT,&out->sets[0]);
    VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=out->sets};
    if(result==VK_SUCCESS)result=vkCreatePipelineLayout(g_vk.dev,&info,NULL,&out->layout);
    if(result==VK_SUCCESS)result=vk_pipeline_compute_build(lease.module,out->layout,recipe->local,&out->pipeline);
    vk_shader_module_release(&lease);return result;
}
int main(int argc,char **argv) {
    assert(argc==3 && !chdir(argv[1]));
    const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
    CHECK(vkCreateInstance(&instance,NULL,&g_vk.inst));
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=report};
    VkDebugUtilsMessengerEXT messenger;CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkCreateDebugUtilsMessengerEXT"))(g_vk.inst,&debug,NULL,&messenger));
    uint32_t count=8;VkPhysicalDevice devices[8];CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&count,devices));assert(count);g_vk.phys=devices[0];
    vkGetPhysicalDeviceProperties(g_vk.phys,&g_vk.props);
    VkPhysicalDeviceVulkan13Features v13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&v13};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&v12};vkGetPhysicalDeviceFeatures2(g_vk.phys,&features);
    assert(v13.maintenance4);
    g_vk.shader_int64=features.features.shaderInt64;g_vk.image_write_without_format=features.features.shaderStorageImageWriteWithoutFormat;
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&count,families);
    while(g_vk.qfam<count && !(families[g_vk.qfam].queueFlags&VK_QUEUE_COMPUTE_BIT))++g_vk.qfam;assert(g_vk.qfam<count);
    float priority=1;VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&v12,.pEnabledFeatures=&features.features,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue};CHECK(vkCreateDevice(g_vk.phys,&device,NULL,&g_vk.dev));
    VkPipelineCacheCreateInfo cache={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};CHECK(vkCreatePipelineCache(g_vk.dev,&cache,NULL,&g_vk.pcache));
    vk_compile_budget_loading(true);vk_pipeline_warm_start_seed(build,argv[2],143);
    unsigned expected=vk_pipeline_warm_stats().seeded;assert(expected);
    unsigned ticks=0;while(!vk_pipeline_warm_wait(50)){assert(++ticks<2400);}
    VkPipelineWarmStats stats=vk_pipeline_warm_stats();assert(stats.prepared==expected && !stats.missing && !stats.failed && !messages);
    VkPipelineSeedIdentity id=vk_pipeline_seed_identity(143);VkPipelineRecipe *recipes=calloc(4096,sizeof *recipes);
    unsigned n=vk_pipeline_seed_load(argv[2],&id,recipes,4096);assert(n==expected);
    for(unsigned i=0;i<n;++i){VkPipelineWarmProduct product;assert(vk_pipeline_warm_take(&recipes[i],&product));vk_pipeline_warm_discard(&product);}
    free(recipes);vk_pipeline_warm_stop();vk_shader_module_cache_reset();
    vkDestroyPipelineCache(g_vk.dev,g_vk.pcache,NULL);vkDestroyDevice(g_vk.dev,NULL);
    ((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkDestroyDebugUtilsMessengerEXT"))(g_vk.inst,messenger,NULL);vkDestroyInstance(g_vk.inst,NULL);
    printf("PASS: %u actual compute seeds rebuilt and taken on %s; validation messages=%u (pipeline creation only)\n",expected,g_vk.props.deviceName,messages);
}
