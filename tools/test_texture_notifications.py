"""Run the production render-thread notification while the NVN registry is locked."""
from pathlib import Path
import os
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
text=(root/'runtime/nvn/vulkan/textures.c').read_text()
start=text.index('void vk_texture_memory_written(')
helper=text[start:text.index('\n}\n',start)+3]
source=r'''
#define _GNU_SOURCE
#include "runtime/nvn/vulkan/vk.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
static NvnTexture textures[3];
static NvnTexture *g_live[]={&textures[0],&textures[1],&textures[2]};
static int g_nlive=3;
static pthread_mutex_t object_registry=PTHREAD_MUTEX_INITIALIZER;
uint64_t tegra_total_size(const TexLayout *layout) { return layout->width; }
// Calling the game-thread notifier here would recreate the observed lock cycle.
void nvn_texture_cpu_written(uint64_t addr,uint64_t size) {
    (void)addr;(void)size;pthread_mutex_lock(&object_registry);pthread_mutex_unlock(&object_registry);
}
'''+helper+r'''
static void *render(void *arg) { (void)arg;vk_texture_memory_written(0x1080,128);return NULL; }
int main(void) {
    textures[0].addr=0x1000;textures[0].layout.width=256;
    textures[1].addr=0x1080;textures[1].layout.width=256;
    textures[2].addr=0x1100;textures[2].layout.width=256;
    pthread_mutex_lock(&object_registry);
    pthread_t thread;assert(!pthread_create(&thread,NULL,render,NULL));
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=2;
    assert(!pthread_timedjoin_np(thread,NULL,&deadline));
    pthread_mutex_unlock(&object_registry);
    assert(textures[0].cpu_write_epoch==1 && textures[1].cpu_write_epoch==1 && !textures[2].cpu_write_epoch);
    vk_texture_memory_written(UINT64_MAX-3,8);vk_texture_memory_written(0x1000,0);
    assert(textures[0].cpu_write_epoch==1 && textures[1].cpu_write_epoch==1);
    puts("PASS: render notification completes under held object lock; overlapping descriptors invalidated only");
}
'''
unit=out/'texture-notification.c';unit.write_text(source)
binary=out/'test-texture-notification'
subprocess.run([os.environ.get('CC','clang'),'-D__ANDROID__','-O1','-g','-fsanitize=address,undefined',
    '-I'+str(root),'-I'+str(root/'android/deps/out/include'),'-I'+str(root/'android/deps/out/volk'),
    str(unit),'-pthread','-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True,timeout=5)
