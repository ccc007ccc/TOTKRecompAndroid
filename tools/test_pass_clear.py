"""Execute the actual pass constructor, scissor and clear dispatch under ASan/UBSan."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / '.work/pass-clear-tests').resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()

def block(start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]

rt = block('typedef struct {\n    NvnTexture *color[8];', 'typedef struct { uint64_t addr, size; } Range;')
begin = block('static VkRect2D scissor_rect(void);', 'static void pass_size(')
scissor = block('static void pass_size(', 'static void apply_scissor(')
clear = block('static void pass_clear_note(', '// ================================================================== programs')
preamble = r'''
#include "runtime/nvn/vulkan/vk.h"
#include "runtime/nvn/vulkan/pass_clear.h"
#include "runtime/diagnostics_build.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
''' + rt + r'''
static struct { RtState rt; bool rt_dirty; uint32_t channel_mask; int32_t scissor[16][4]; float viewport[16][4]; } st;
static bool in_pass, pipeline_dirty, g_debug_frame, g_gpuprof_frame;
static uint64_t g_test_scene_frame, g_scene_frame;
static int dl_on, pass_written, wb_npass;
static uint32_t pass_dup_null, pass_ncolor, pass_samples, pass_w, pass_h, pass_dump_layer;
static VkFormat pass_color[8], pass_depth, pass_stencil;
static float pass_sx=1, pass_sy=1, vp_sx=1, vp_sy=1, vp_ox, vp_oy;
static NvnTexture *wb_pass[8];
NvnTexture *g_pass_t0; int g_pass_layer;
static TexBackend *pass_dump_b;
static struct { bool dirty; } frame_capture;
static struct { unsigned known; } ss;
enum { SS_SETS=1 };
static unsigned stat_passes, begins, ends, barriers, attachment_clears, draws, dirty_marks, stencil_mask;
static bool automatic_first_write;
static VkRenderingAttachmentInfo colors[8], depth_attachment, stencil_attachment;
static uint32_t captured_layers;
static VkRect2D captured_area;
static VkClearAttachment last_clear[2];
static VkClearRect last_rect;
#define AB(x) 0
static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }
static uint64_t SDL_GetTicksNS(void) { return 1; }
static bool test_session_enabled(void) { return false; }
static uint64_t renderer_present_count(void) { return 1; }
void vk_end_pass(void) { if (in_pass) ++ends; in_pass=false; }
bool renderer_texture_scalable(NvnTexture *t) { (void)t; return false; }
void vk_make_scalable_like(NvnTexture *t, NvnTexture *like) { (void)t; (void)like; }
uint8_t vk_texture_scale_group(NvnTexture *t) { (void)t; return RS_NONE; }
void renderer_flush(void) {}
void vk_full_barrier(VkCommandBuffer cb) { (void)cb; ++barriers; }
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)1; }
static void dl_note(const char *fmt, ...) { (void)fmt; }
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) { (void)cb; (void)fmt; }
void renderer_mark_gpu_owned(NvnTexture *t) { ((TexBackend*)t->backend)->gpu_owned=true; }
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int type) {
    (void)render; (void)type;
    unsigned mip=has_view && (v->setmask&1)?v->base_level:0;
    unsigned layer=has_view && (v->setmask&2)?v->min_layer:0;
    return (VkImageView)(uintptr_t)(t->debug_id+100*mip+10*layer);
}
VkFormat vk_format(uint32_t f) { return (VkFormat)f; }
static bool renderer_texture_first_write(NvnTexture *t) { (void)t; return automatic_first_write; }
static bool renderer_texture_level_fresh(NvnTexture *t) { (void)t; return false; }
static bool renderer_texture_frame_fresh(NvnTexture *t) { (void)t; return false; }
static bool wb_eligible(NvnTexture *t, TexBackend *b, uint32_t lv) { (void)t; (void)b; (void)lv; return false; }
static void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) { TexBackend *b=t->backend; *sx=b->sx; *sy=b->sy; }
bool renderer_format_is_depth(uint32_t f) { return f==VK_FORMAT_D32_SFLOAT_S8_UINT || f==VK_FORMAT_D32_SFLOAT; }
static bool capture_enabled(void) { return false; }
static void vk_diag_mark_dirty(void) { ++dirty_marks; }
static void debug_target_begin(const VkRenderingAttachmentInfo *ca) { (void)ca; }
static void ss_forget(void) { ss.known=0; }
uint64_t nvn_watch_texaddr(void) { return 0; }
uint64_t tegra_total_size(const TexLayout *t) { (void)t; return 0; }
void vk_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat *formats, VkFormat ds, VkSampleCountFlagBits samples) {
    (void)cb; (void)formats; (void)ds; (void)samples; ++begins;
    memset(colors,0,sizeof colors); memset(&depth_attachment,0,sizeof depth_attachment); memset(&stencil_attachment,0,sizeof stencil_attachment);
    memcpy(colors,ri->pColorAttachments,ri->colorAttachmentCount*sizeof *colors);
    if(ri->pDepthAttachment)depth_attachment=*ri->pDepthAttachment;
    if(ri->pStencilAttachment)stencil_attachment=*ri->pStencilAttachment;
    captured_layers=ri->layerCount; captured_area=ri->renderArea;
}
static unsigned vk_diag_group(void) { return 0; }
static bool vk_diag_active(void) { return false; }
static void vk_diag_log(const char *fmt, ...) { (void)fmt; }
static bool widescreen_ui_centred(void) { return false; }
static float widescreen_hud_shift(float x) { (void)x; return 0; }
static bool framepace_in_menu(void) { return false; }
static float widescreen_factor(void) { return 1; }
static VkPipeline clear_pipeline(int i, int k, uint32_t m, bool d, bool s) { (void)i;(void)k;(void)m;(void)d;(void)s; return (VkPipeline)1; }
static void dyn_defaults(VkCommandBuffer cb) { (void)cb; }
static VkPipelineLayout pc_layout(void) { return (VkPipelineLayout)1; }
void vkCmdClearAttachments(VkCommandBuffer cb,uint32_t n,const VkClearAttachment *a,uint32_t nr,const VkClearRect *r) {
    (void)cb; assert(n<=2 && nr==1); ++attachment_clears; memcpy(last_clear,a,n*sizeof *a); last_rect=*r;
}
void vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p) { (void)c;(void)b;(void)p; }
void vkCmdSetDepthTestEnable(VkCommandBuffer c,VkBool32 v) { (void)c;(void)v; }
void vkCmdSetDepthWriteEnable(VkCommandBuffer c,VkBool32 v) { (void)c;(void)v; }
void vkCmdSetStencilTestEnable(VkCommandBuffer c,VkBool32 v) { (void)c;(void)v; }
void vkCmdSetStencilOp(VkCommandBuffer c,VkStencilFaceFlags f,VkStencilOp a,VkStencilOp b,VkStencilOp d,VkCompareOp e) { (void)c;(void)f;(void)a;(void)b;(void)d;(void)e; }
void vkCmdSetStencilWriteMask(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t m) { (void)c;(void)f; stencil_mask=m; }
void vkCmdSetStencilReference(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t r) { (void)c;(void)f;(void)r; }
void vkCmdSetViewport(VkCommandBuffer c,uint32_t f,uint32_t n,const VkViewport *v) { (void)c;(void)f;(void)n;(void)v; }
void vkCmdSetScissor(VkCommandBuffer c,uint32_t f,uint32_t n,const VkRect2D *v) { (void)c;(void)f;(void)n;(void)v; }
void vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags s,uint32_t o,uint32_t n,const void *p) { (void)c;(void)l;(void)s;(void)o;(void)n;(void)p; }
void vkCmdDraw(VkCommandBuffer c,uint32_t a,uint32_t b,uint32_t d,uint32_t e) { (void)c;(void)a;(void)b;(void)d;(void)e; ++draws; }
'''
test = r'''
static TexBackend cb, db;
static NvnTexture ct, dt;
static const uint32_t value[4]={0x81234567,0xfedcba98,0,0xffffffff};
static void reset(void) {
    memset(&st,0,sizeof st); in_pass=false; begins=ends=barriers=attachment_clears=draws=dirty_marks=0;
    automatic_first_write=false;
    cb=(TexBackend){.format=VK_FORMAT_R32_UINT,.base_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY,.width=64,.height=32,.levels=3,.layers=4,.samples=1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.sx=2,.sy=2};
    db=cb; db.format=VK_FORMAT_D32_SFLOAT_S8_UINT; db.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    ct=(NvnTexture){.width=32,.height=16,.format=cb.format,.backend=&cb,.debug_id=1};
    dt=ct; dt.backend=&db; dt.format=db.format; dt.debug_id=2;
    st.rt.ncolor=1; st.rt.color[0]=&ct; st.rt.depth=&dt; st.channel_mask=15;
    st.scissor[0][2]=32; st.scissor[0][3]=16; st.rt_dirty=true;
}
static void color_clear(uint32_t mask) { clear(0,2,value,mask,false,0,false,0,0); }
static void ds_clear(bool depth,bool stencil,uint32_t mask) { clear(-1,0,NULL,0,depth,0.25f,stencil,0x37,mask); }
int main(void) {
    unsetenv("LARECOMP_VK_LOADOP_CLEAR"); unsetenv("LARECOMP_LOG_PASS_CLEARS");
    reset(); color_clear(15); assert(attachment_clears==1 && colors[0].loadOp==VK_ATTACHMENT_LOAD_OP_LOAD);
    setenv("LARECOMP_VK_LOADOP_CLEAR","0",1);
    reset(); ds_clear(true,true,255); assert(attachment_clears==1);
    setenv("LARECOMP_VK_LOADOP_CLEAR","1",1);
    reset(); automatic_first_write=true; color_clear(15);
#if TOTK_DIAGNOSTICS
    assert(!attachment_clears && !draws && colors[0].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR);
    assert(!memcmp(&colors[0].clearValue.color,value,16));
#else
    assert(attachment_clears==1);
#endif
    assert(begins==1 && barriers==1 && dirty_marks==1 && colors[0].storeOp==VK_ATTACHMENT_STORE_OP_STORE);
    reset(); ds_clear(true,false,0);
#if TOTK_DIAGNOSTICS
    assert(!attachment_clears && depth_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && depth_attachment.clearValue.depthStencil.depth==0.25f);
    assert(stencil_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_LOAD);
#else
    assert(attachment_clears==1);
#endif
    reset(); ds_clear(false,true,255);
#if TOTK_DIAGNOSTICS
    assert(!attachment_clears && stencil_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && stencil_attachment.clearValue.depthStencil.stencil==0x37);
    assert(depth_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_LOAD);
#else
    assert(attachment_clears==1);
#endif
    reset(); ds_clear(true,true,255);
    assert(depth_attachment.storeOp==VK_ATTACHMENT_STORE_OP_STORE && stencil_attachment.storeOp==VK_ATTACHMENT_STORE_OP_STORE);
#if TOTK_DIAGNOSTICS
    assert(!attachment_clears && depth_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && stencil_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR);
#else
    assert(attachment_clears==1);
#endif
    reset(); begin_pass(); ds_clear(true,true,255); assert(begins==1 && attachment_clears==1);
    reset(); begin_pass(); st.rt_dirty=true; ds_clear(true,true,255); assert(begins==2 && ends==1 && barriers==2);
#if TOTK_DIAGNOSTICS
    assert(!attachment_clears);
#else
    assert(attachment_clears==1);
#endif
    reset(); st.scissor[0][0]=2; st.scissor[0][2]=30; color_clear(15);
    assert(attachment_clears==1 && last_rect.rect.offset.x==4 && last_rect.rect.extent.width==60);
    reset(); st.scissor[0][2]=0; color_clear(15); assert(!attachment_clears && !draws);
    reset(); color_clear(3); assert(draws==1 && !attachment_clears);
    reset(); ds_clear(true,true,0x0f); assert(draws==1 && stencil_mask==0x0f && !attachment_clears);
    reset(); db.width=128; ds_clear(true,true,255); assert(attachment_clears==1);
    reset(); cb.width=128; color_clear(15); assert(attachment_clears==1);
    reset(); cb.base_type=VK_IMAGE_VIEW_TYPE_3D; color_clear(15); assert(attachment_clears==1);
    reset(); st.rt.ncolor=2; st.rt.color[1]=&ct; color_clear(15);
    assert(!colors[1].imageView && captured_layers==1);
    reset(); st.rt.ncolor=2; st.rt.color[1]=&ct;
    clear(1,2,value,15,false,0,false,0,0); assert(!attachment_clears && !draws);
    reset(); db.aspect=VK_IMAGE_ASPECT_DEPTH_BIT; db.format=VK_FORMAT_D32_SFLOAT; dt.format=db.format;
    ds_clear(false,true,255); assert(!attachment_clears && !stencil_attachment.imageView);
    reset(); st.rt.has_cview[0]=st.rt.has_dview=1;
    st.rt.cview[0]=(NvnTextureView){.setmask=3,.base_level=1,.min_layer=2,.num_layers=2}; st.rt.dview=st.rt.cview[0];
    st.scissor[0][2]=16; st.scissor[0][3]=8; ds_clear(true,true,255);
    assert(captured_area.extent.width==32 && captured_area.extent.height==16 && captured_layers==1);
    assert(depth_attachment.imageView==(VkImageView)122);
#if TOTK_DIAGNOSTICS
    assert(!attachment_clears && depth_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR);
#else
    assert(attachment_clears==1);
#endif
    puts("PASS: production pass/scissor/clear dispatch; masks, target changes, first-write override, scaling, mips/layers, dropped slots, STORE/barrier and diagnostic isolation");
}
'''
unit = out / 'pass.c'
unit.write_text(preamble + begin + scissor + clear + test)
for diagnostics in (0, 1):
    binary = out / f'pass-{diagnostics}'
    subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', f'-DTOTK_DIAGNOSTICS={diagnostics}',
                    '-I'+str(root), '-I'+str(root/'android/deps/out/include'), str(unit), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
