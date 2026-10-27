#include <volk.h>
#include <SDL3/SDL.h>
#include "runtime/nvn/vulkan/vk.h"
#include "runtime/nvn/vulkan/flicker.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <adrenotools/driver.h>
#include "runtime/nvn/vulkan/renderdoc_bridge.h"
#include <sys/stat.h>
VkG g_vk;
static VkCommandBuffer command;
static uint64_t complete;
static bool submitted;
void renderer_submit(void) {
 assert(vkEndCommandBuffer(command)==VK_SUCCESS);
 VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
 assert(vkQueueSubmit(g_vk.queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
 assert(vkQueueWaitIdle(g_vk.queue)==VK_SUCCESS);complete++;submitted=true;
}
void vk_wait_seq(uint64_t seq) { assert(complete>=seq); }
static const char *output;
const char *rt_data_root(void) { return output; }
VkCommandBuffer vk_cmd(void) { return command; }
uint64_t vk_pending_seq(void) { return complete+1; }
uint64_t vk_completed_seq(void) { return complete; }
void vk_full_barrier(VkCommandBuffer cb) {
 VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
 .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
}
void vk_readback_barrier(VkCommandBuffer cb) {
 VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,NULL,0,NULL);
}
extern void vk_flicker_frame(TexBackend *,uint64_t,bool);
static void step(TexBackend *im,uint64_t frame,float color,bool stop) {
 flicker_next_frame(true); submitted=false;
 assert(vkResetCommandBuffer(command,0)==VK_SUCCESS);
 VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
 VkClearColorValue clear={.float32={color,color,color,1}};
 VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 vk_full_barrier(command);
 vkCmdClearColorImage(command,im->image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
 if(stop)flicker_stop();
 vk_flicker_frame(im,frame,true);
 if (!submitted) renderer_submit();
}
int main(int argc,char **argv) {
 assert(argc==2);output=argv[1];mkdir(output,0755);
 const char *drv=getenv("REPLAY_DRIVER"),*hooks=getenv("REPLAY_HOOKS");
 assert(drv&&hooks); char folder[1024];snprintf(folder,sizeof folder,"%s",drv);
 char *slash=strrchr(folder,'/');assert(slash);char name[128];snprintf(name,sizeof name,"%s",slash+1);slash[1]=0;
 void *lib=adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,NULL,hooks,folder,name,NULL,NULL);assert(lib);
 PFN_vkGetInstanceProcAddr proc=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");assert(proc);
 char error[512];proc=rd_layer_load(proc,getenv("CAPTURE_LAYER"),error,sizeof error);
 if(!proc){fprintf(stderr,"%s\n",error);return 1;}volkInitializeCustom(proc);
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
 VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
 assert(vkCreateInstance(&ci,NULL,&g_vk.inst)==VK_SUCCESS);volkLoadInstance(g_vk.inst);
 uint32_t n=1;assert(vkEnumeratePhysicalDevices(g_vk.inst,&n,&g_vk.phys)==VK_SUCCESS);
 vkGetPhysicalDeviceProperties(g_vk.phys,&g_vk.props);printf("GPU: %s\n",g_vk.props.deviceName);
 VkQueueFamilyProperties families[32];n=32;vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&n,families);
 while(!(families[g_vk.qfam].queueFlags&VK_QUEUE_GRAPHICS_BIT))g_vk.qfam++;
 float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,.queueCount=1,.pQueuePriorities=&priority};
 VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
 assert(vkCreateDevice(g_vk.phys,&dci,NULL,&g_vk.dev)==VK_SUCCESS);volkLoadDevice(g_vk.dev);vkGetDeviceQueue(g_vk.dev,g_vk.qfam,0,&g_vk.queue);
 VmaVulkanFunctions funcs={.vkGetInstanceProcAddr=vkGetInstanceProcAddr,.vkGetDeviceProcAddr=vkGetDeviceProcAddr};
 VmaAllocatorCreateInfo ai={.physicalDevice=g_vk.phys,.device=g_vk.dev,.instance=g_vk.inst,.vulkanApiVersion=VK_API_VERSION_1_3,.pVulkanFunctions=&funcs};
 assert(vmaCreateAllocator(&ai,&g_vk.vma)==VK_SUCCESS);
 VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
 assert(vkCreateCommandPool(g_vk.dev,&pci,NULL,&g_vk.pool)==VK_SUCCESS);
 VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=g_vk.pool,.commandBufferCount=1,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY};
 assert(vkAllocateCommandBuffers(g_vk.dev,&ca,&command)==VK_SUCCESS);
 TexBackend im={.width=1280,.height=720,.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=1};
 VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=im.format,.extent={1280,720,1},.mipLevels=1,.arrayLayers=1,.samples=1,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
 VmaAllocationCreateInfo ma={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};assert(vmaCreateImage(g_vk.vma,&ii,&ma,&im.image,&im.mem,NULL)==VK_SUCCESS);
 VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
 VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=im.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
 assert(vkEndCommandBuffer(command)==VK_SUCCESS);VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};assert(vkQueueSubmit(g_vk.queue,1,&si,0)==VK_SUCCESS);vkQueueWaitIdle(g_vk.queue);
 // Two stable frames must not produce an event; the third is the changed frame.
 flicker_start(); assert(flicker_status().active);
 step(&im,1,0.5f,false); step(&im,2,0.5f,false);
 assert(flicker_status().saved==0 && !flicker_status().saving);
 step(&im,3,0.0f,false);
 for(int i=0;i<2000 && flicker_status().saving;i++) SDL_Delay(2);
 FlickerStatus st=flicker_status();assert(st.saved==1);
 printf("PAIR %s\n",st.directory);
 flicker_stop();flicker_next_frame(true);flicker_reset();
 // Single-shot works without continuous monitoring or any change threshold.
 flicker_single(); assert(flicker_status().single_pending);
 step(&im,7,0.25f,false);
 for(int i=0;i<2000 && flicker_status().saving;i++) SDL_Delay(2);
 st=flicker_status();assert(st.saved==1 && !st.active && !st.single_pending);
 printf("SINGLE %s\n",st.directory);flicker_reset();
 // Closing the menu/discarding and stopping unchanged frames save nothing.
 flicker_start(); step(&im,10,0.5f,false); step(&im,11,0.5f,false);
 flicker_stop();flicker_next_frame(true);assert(flicker_status().saved==0);flicker_reset();
 flicker_start();step(&im,20,0.5f,false);flicker_next_frame(true);
 vk_flicker_frame(NULL,21,false);flicker_stop();flicker_next_frame(false);
 assert(flicker_status().saved==0);flicker_reset();
 vmaDestroyImage(g_vk.vma,im.image,im.mem);vkDestroyCommandPool(g_vk.dev,g_vk.pool,NULL);vmaDestroyAllocator(g_vk.vma);vkDestroyDevice(g_vk.dev,NULL);vkDestroyInstance(g_vk.inst,NULL);
 puts("PASS: paired GPU capture, stable-frame rejection, manual capture, stop, discard and restart");
}
