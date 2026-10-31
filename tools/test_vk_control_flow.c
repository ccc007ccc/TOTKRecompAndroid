// Execute the actual translated control-flow bodies with different inputs in
// every lane. Expected values are independent of the structurer/state machine.
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r);exit(1); } } while(0)
static unsigned messages;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    if(severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){++messages;fprintf(stderr,"%s\n",data->pMessage);}
    return VK_FALSE;
}
int main(int argc,char **argv) {
    assert(argc==2);
    const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation={.sType=VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&sync};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pNext=&validation,.pApplicationInfo=&app,
        .enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
    VkInstance instance;CHECK(vkCreateInstance(&ii,NULL,&instance));
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=report};
    VkDebugUtilsMessengerEXT messenger;
    CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"))(instance,&debug,NULL,&messenger));
    uint32_t count=8;VkPhysicalDevice devices[8];CHECK(vkEnumeratePhysicalDevices(instance,&count,devices));assert(count);
    VkPhysicalDevice gpu=devices[0];VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(gpu,&props);
    VkPhysicalDeviceFeatures have;vkGetPhysicalDeviceFeatures(gpu,&have);assert(have.shaderInt64);
    VkPhysicalDeviceFeatures enabled={.shaderInt64=VK_TRUE};
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families);
    unsigned family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;assert(family<count);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVulkan13Features v13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.maintenance4=VK_TRUE};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&v13,.pEnabledFeatures=&enabled,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device;CHECK(vkCreateDevice(gpu,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=8192,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;CHECK(vkCreateBuffer(device,&bi,NULL,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(gpu,&memory);
    unsigned type=0;VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;
    assert(type<memory.memoryTypeCount);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    VkDeviceMemory allocation;CHECK(vkAllocateMemory(device,&ai,NULL,&allocation));CHECK(vkBindBufferMemory(device,buffer,allocation,0));
    uint32_t *data;CHECK(vkMapMemory(device,allocation,0,8192,0,(void**)&data));
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
                                           {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout layout;CHECK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
    VkPipelineLayout pipeline_layout;CHECK(vkCreatePipelineLayout(device,&pli,NULL,&pipeline_layout));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool descriptors;CHECK(vkCreateDescriptorPool(device,&dpi,NULL,&descriptors));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dai,&set));
    VkDescriptorBufferInfo buffers[]={{buffer,0,1024},{buffer,4096,1024}};
    for(unsigned i=0;i<2;++i){VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};vkUpdateDescriptorSets(device,1,&w,0,NULL);}
    VkCommandPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    VkCommandPool pool;CHECK(vkCreateCommandPool(device,&ci,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cai,&command));
    unsigned checks=0;
    for(unsigned fixture=4;fixture<12;++fixture)for(unsigned stacks=0;stacks<2;++stacks)
    for(unsigned structured=0;structured<2;++structured)for(unsigned explicit_next=0;explicit_next<2;++explicit_next) {
        char path[2048];snprintf(path,sizeof path,"%s/gpu-flow-%u-%u-%u-%u.comp.spv",argv[1],fixture,stacks,structured,explicit_next);
        FILE *file=fopen(path,"rb");assert(file && !fseek(file,0,SEEK_END));long bytes=ftell(file);assert(bytes>20 && !(bytes%4));rewind(file);
        uint32_t *words=malloc(bytes);assert(words && fread(words,1,bytes,file)==(size_t)bytes && !fclose(file));
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=words};
        VkShaderModule module;CHECK(vkCreateShaderModule(device,&mi,NULL,&module));free(words);
        VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=pipeline_layout,
            .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
        VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
        for(unsigned i=0;i<256;++i){data[i]=i;data[1024+i]=0xdeadbeef;}
        CHECK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);vkCmdDispatch(command,4,1,1);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        CHECK(vkEndCommandBuffer(command));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        for(unsigned i=0;i<256;++i){unsigned expected=fixture==4?20:fixture==6?12:i==0?41:i==1?11:i==2?31:21;
            if(fixture==7||fixture==11)expected=i;
            if(fixture==8)expected=i<<8;
            if(fixture==9)expected=7348;
            if(fixture==10)expected=i<4?77:i<8?33:0;
            if(data[1024+i]!=expected){fprintf(stderr,"%s input %u: %u != %u\n",path,i,data[1024+i],expected);return 1;}++checks;}
        vkDestroyPipeline(device,pipeline,NULL);vkDestroyShaderModule(device,module,NULL);
    }
    vkDestroyCommandPool(device,pool,NULL);vkDestroyDescriptorPool(device,descriptors,NULL);
    vkDestroyPipelineLayout(device,pipeline_layout,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);
    vkUnmapMemory(device,allocation);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,allocation,NULL);vkDestroyDevice(device,NULL);
    ((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,NULL);vkDestroyInstance(instance,NULL);
    assert(!messages);printf("PASS: %u divergent control-flow GPU values on %s, zero validation messages\n",checks,props.deviceName);
}
