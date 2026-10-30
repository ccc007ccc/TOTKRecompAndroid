// Golden outputs captured from the old compiled loader before extracting its
// key/target logic. These must preserve all existing exact SPIR-V keys.
#include "../runtime/nvn/vulkan/shader_source_key.h"
#include <assert.h>
#include <stdio.h>
VkG g_vk;
int main(void){
    const uint64_t golden[2][3]={{0x790a0a68f235e045ull,0x7ecf254633185acaull,0x0480a5bf971f783eull},
                               {0x5ba23fab95ab11ceull,0x81c39e24a4ff62f9ull,0x33555b00669adf95ull}};
    const int stages[]={0,1,5};
    for(unsigned optimized=0;optimized<2;++optimized){
        if(optimized)setenv("LARECOMP_VK_OPT","1",1);else unsetenv("LARECOMP_VK_OPT");
        for(unsigned i=0;i<3;++i)assert(vk_shader_source_key("#version 450\nvoid main(){}\n",stages[i])==golden[optimized][i]);
#ifdef __ANDROID__
        g_vk.api=VK_API_VERSION_1_1;
        for(unsigned i=0;i<3;++i)assert(vk_shader_source_key("#version 450\nvoid main(){}\n",stages[i])==(golden[optimized][i]^0x0000200000000000ull));
        g_vk.api=0;
#endif
    }
    puts("PASS: old exact source keys preserved for VS/FS/CS, normal/optimized modes and actual/fallback target API");
}
