"""Execute the production change-detection decision, stopping at the Vulkan upload boundary."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
text = (root / 'runtime/nvn/vulkan/textures.c').read_text()
hashes = text[text.index('static uint64_t full_hash_budget,'):text.index('static pthread_mutex_t pending_lock')]
helpers = text[text.index('static uint8_t *upload_scratch;'):text.index('static void upload_texture(')]
decision = text[text.index('static void upload_texture('):text.index('    VkCommandBuffer cb = vk_upload_cmd();', text.index('static void upload_texture('))]
unit = r'''
#define _POSIX_C_SOURCE 200809L
#include "runtime/nvn/vulkan/vk.h"
#include "runtime/diagnostics_build.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t frame=1, tick=1;
static unsigned uploads, style;
uint64_t vk_frame_counter(void) { return frame; }
uint64_t SDL_GetTicksNS(void) { return tick; }
int vk_pages_changed(uint64_t a,uint64_t s,uint32_t since,uint32_t *now) {
    (void)a;(void)s;(void)since;(void)now;return -1;
}
typedef struct { VkFormat vf; } VkFmt;
static VkFmt fmt_of(uint32_t f) { (void)f; return (VkFmt){VK_FORMAT_R8_UNORM}; }
int texpatch_identify(const NvnTexture *t) { (void)t; return -1; }
uint32_t texpatch_generation(void) { return style; }
''' + hashes + helpers + decision + r'''
    ++uploads;
}
static void next_check(TexBackend *b) { do { ++frame; } while ((frame+((uintptr_t)b>>6))&7); }
static void change(NvnTexture *t,TexBackend *b,uint8_t *data,bool notify) {
    ++data[0]; if (notify) ++t->cpu_write_epoch; else next_check(b);
    unsigned before=uploads; upload_texture(t,b); assert(uploads==before+1);
}
int main(void) {
    setenv("LARECOMP_TEXTURE_TRACE","1",1);
    if (!getenv("LARECOMP_TEXTURE_FLUSH_TRACKING")) setenv("LARECOMP_TEXTURE_FLUSH_TRACKING","1",1);
    uint8_t *data=calloc(1,17<<20); assert(data);
    FormatInfo fmt={.bw=1,.bh=1,.bpb=1}; NvnPool pool={.flags=0x24};
    NvnTexture t={.addr=(uintptr_t)data,.pool=&pool,.target=1,.width=5120,.height=5120,
        .depth=1,.levels=7,.storage_size=17<<20,.layout={.fmt=&fmt}};
    TexBackend b={.format=VK_FORMAT_R8_UNORM};
    upload_texture(&t,&b); assert(uploads==1 && b.uploaded && !b.dynamic);
    if (atoi(getenv("LARECOMP_TEXTURE_FLUSH_TRACKING")) == 0) {
        change(&t,&b,data,true); assert(b.dynamic);
        free(data);
        puts("PASS: disabled flush tracking retains full checks after notified changes");
        return 0;
    }
    change(&t,&b,data,true); assert(!b.dynamic && b.cpu_write_epoch==t.cpu_write_epoch);
    uint64_t full=prof_tex_full_bytes;
    for(unsigned i=0;i<120;++i) { ++frame; upload_texture(&t,&b); }
    assert(prof_tex_full_bytes==full && uploads==2); // No repeated 17 MiB hashes after a notified update.
    ++t.cpu_write_epoch; upload_texture(&t,&b); assert(uploads==2 && !b.dynamic); // Same bytes: acknowledged, no upload.
    change(&t,&b,data,false); assert(b.dynamic); // Unannounced writes retain the conservative path.
    // Stable large dynamic textures still read all bytes on every checked frame, including unsampled offsets.
    for (unsigned i=0;i<3;++i) {
        ++frame;
        unsigned old=uploads;
        size_t offset=i==0 ? (17u<<20)-1 : i==1 ? (8u<<20)+3 : 8193;
        data[offset]^=1; upload_texture(&t,&b); assert(uploads==old+1);
        ++frame; old=uploads; upload_texture(&t,&b); assert(uploads==old);
    }
    change(&t,&b,data,true); assert(b.dynamic); // A later notification cannot erase earlier uncertainty.
    b=(TexBackend){.format=VK_FORMAT_R8_UNORM}; pool.flags=0x22;
    upload_texture(&t,&b); change(&t,&b,data,true); assert(b.dynamic); // CPU uncached memory is not flush-only.
    b=(TexBackend){.format=VK_FORMAT_R8_UNORM}; pool.flags=0x24; t.flags=0x10;
    upload_texture(&t,&b); assert(b.dynamic); // Linear dynamic textures retain their explicit policy.
    b=(TexBackend){.format=VK_FORMAT_R8_UNORM}; t.flags=0; t.storage_size=4096;
    upload_texture(&t,&b); unsigned before=uploads;
    data[1]++; // Outside the sampled offsets. Small textures still get periodic full checks.
    frame+=128; next_check(&b); upload_texture(&t,&b); assert(uploads==before+1 && b.dynamic);
    b.tp=1; b.tp_gen=style; ++style; ++frame;
    before=uploads; upload_texture(&t,&b); assert(uploads==before+1 && b.tp_gen==style);
    t.target=10; ++t.cpu_write_epoch; before=uploads; upload_texture(&t,&b); assert(uploads==before);
    // Diagnostic windows may outlive the texture and its pool. Flush the window after both have been freed.
    NvnTexture *gone=malloc(sizeof *gone); NvnPool *gone_pool=malloc(sizeof *gone_pool);
    *gone=t; *gone_pool=pool; gone->pool=gone_pool;
    texture_check_record(gone,&b,4096,false); free(gone_pool); free(gone);
    tick+=3000000000ull; texture_check_record(&t,&b,4096,false);
    free(data);
    for(unsigned i=0;i<8;++i) free(texture_snapshots[i].bytes);
    free(upload_scratch);
    puts("PASS: notified, unchanged, unannounced, uncached, linear, periodic, restyle and diagnostic lifetime");
}
'''
source = out / 'texture-change.c'
source.write_text(unit)
binary = out / 'test-texture-change'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined',
                '-I'+str(root), '-I'+str(root/'android/deps/out/include'), str(source),
                str(root/'runtime/nvn/tegra.c'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=20)
subprocess.run([str(binary)], check=True, timeout=20,
               env={**os.environ, 'LARECOMP_TEXTURE_FLUSH_TRACKING': '0'})
