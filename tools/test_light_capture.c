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
#include "runtime/nvn/vulkan/diagnostic.h"
#include <sys/stat.h>
VkG g_vk;
static VkCommandBuffer command;
static uint64_t complete;
static bool submitted;
static TexBackend depth_image, integer_image;
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
 VkClearDepthStencilValue dc={0.75f,0};
 VkImageSubresourceRange dr={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
 vkCmdClearDepthStencilImage(command,depth_image.image,VK_IMAGE_LAYOUT_GENERAL,&dc,1,&dr);
 unsigned payload[1024];for(int k=0;k<1024;k++)payload[k]=(unsigned)(color*1000)+k;
 unsigned blob=vk_diag_blob(10,0x123400,payload,1ull<<32,sizeof payload);
 assert(blob && vk_diag_blob(10,0x123400,payload,1ull<<32,sizeof payload)==blob);
 vk_diag_log("synthetic frame=%llu cbuf_blob=%u\n",(unsigned long long)frame,blob);
 unsigned n=frame==7?400:1;
 for(unsigned k=0;k<n;k++) {
  vk_diag_image(im,0xabcdef,0,0,0,VK_IMAGE_ASPECT_COLOR_BIT);vk_diag_next_group();
 }
 vk_diag_image(&depth_image,0x987654,8,0,0,VK_IMAGE_ASPECT_DEPTH_BIT);vk_diag_next_group();
 // R32_UINT has blit features, but may not be converted to the RGBA16F thumbnail.
 vk_diag_image(&integer_image,0x112233,0,0,0,VK_IMAGE_ASPECT_COLOR_BIT);vk_diag_next_group();
 if(frame==7){
  for(unsigned k=0;k<2200;k++){payload[0]=k;vk_diag_blob(10,0x100000+k*4096,payload,sizeof payload,sizeof payload);}
  char line[1001];memset(line,'x',1000);line[1000]=0;
  for(unsigned k=0;k<5000;k++)vk_diag_log(" %s\n",line);
  vk_diag_log("critical-late-draw shader_state=0\n");
 }
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
 volkInitializeCustom(proc);
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
 TexBackend im={.width=1280,.height=720,.depth=1,.levels=1,.layers=1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=1};
 VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=im.format,.extent={1280,720,1},.mipLevels=1,.arrayLayers=1,.samples=1,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
 VmaAllocationCreateInfo ma={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};assert(vmaCreateImage(g_vk.vma,&ii,&ma,&im.image,&im.mem,NULL)==VK_SUCCESS);
 depth_image=(TexBackend){.width=1280,.height=720,.depth=1,.levels=1,.layers=1,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT,.format=VK_FORMAT_D32_SFLOAT,.samples=1};
 ii.format=depth_image.format;assert(vmaCreateImage(g_vk.vma,&ii,&ma,&depth_image.image,&depth_image.mem,NULL)==VK_SUCCESS);
 integer_image=im;integer_image.format=VK_FORMAT_R32_UINT;
 ii.format=integer_image.format;assert(vmaCreateImage(g_vk.vma,&ii,&ma,&integer_image.image,&integer_image.mem,NULL)==VK_SUCCESS);
 VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
 VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=im.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
 b.image=integer_image.image;
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
 b.image=depth_image.image;b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
 assert(vkEndCommandBuffer(command)==VK_SUCCESS);VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};assert(vkQueueSubmit(g_vk.queue,1,&si,0)==VK_SUCCESS);vkQueueWaitIdle(g_vk.queue);
 // Two stable frames must not produce an event; the third is the changed frame.
 flicker_start(); assert(flicker_status().active && !flicker_status().full);
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
 flicker_start();step(&im,30,0.5f,true);flicker_next_frame(false);assert(flicker_status().saved==0);flicker_reset();
 vmaDestroyImage(g_vk.vma,integer_image.image,integer_image.mem);vmaDestroyImage(g_vk.vma,depth_image.image,depth_image.mem);vmaDestroyImage(g_vk.vma,im.image,im.mem);vkDestroyCommandPool(g_vk.dev,g_vk.pool,NULL);vmaDestroyAllocator(g_vk.vma);vkDestroyDevice(g_vk.dev,NULL);vkDestroyInstance(g_vk.inst,NULL);
 puts("PASS: light stage/depth snapshots, integer rejection, bounded inputs/log, stable rejection, before/after, single, stop and restart");
}
