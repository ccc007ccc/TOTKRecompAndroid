// Actual stage_set + descriptor-update module, with immutable driver-set doubles.
// The fixture resolves changing inputs on every call, including cache hits.
#define TOTK_DIAGNOSTICS 1
#include "../runtime/nvn/vulkan/shaders.h"
#include "../runtime/nvn/vulkan/descriptor_update.h"
#include "../runtime/diagnostics_build.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_BIND 96
#define GMAP_BUFFER_INDEX 20
#define GFAST_BUFFER_INDEX 21
#define GMISS_BUFFER_INDEX 22
#define VERTEX_DEBUG_BINDING 90
#define DEDUPE_RSCALE_F 0
#define DEDUPE_RSCALE_V 1
#define DEDUPE_TINT 2
#define H(T,x) ((T)(uintptr_t)(x))
VkG g_vk;
static int prof;
static uint64_t prof_cbuf, prof_dsw, prof_set_hits, prof_set_miss;
static float tex_sc[2][64][2], g_mirror_w;
static uint32_t tex_wh[2][64][2], vertex_debug_dynamic_cbufs, gmap_tab[16];
static VkDescriptorBufferInfo vertex_debug_cbufs[17];
static VkBuffer gmem_buf;
static uint64_t seq = 1, uniform_offset = 256;
static uint32_t resource_change;
static unsigned sets, creates, destroys, writes, resolves;
static bool fail_create, fail_allocate;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size) {
    if (fail_allocate && n == 1 && size == sizeof(VkDescriptorUpdate)) return NULL;
    return __real_calloc(n, size);
}
typedef struct { uint32_t count; VkDescriptorUpdateTemplateEntry entries[MAX_BIND]; } Template;
typedef struct { VkDescriptorValue values[96]; VkDescriptorType types[96]; bool written[96], sealed; } DriverSet;
static DriverSet stored[64];
void rt_fatal_message(const char *format, ...) { (void)format; abort(); }
void rt_fatal_trace(void) { abort(); }
uint64_t plat_now_ns(void) { return 1; }
int vk_ab(const char *what) { (void)what; return -1; }
static bool is_ubo(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; }
#include "descriptor_plan.inc"
typedef struct {
    VkBindingInfo binds[MAX_BIND]; int nbinds;
    DescriptorPlan descriptor_plans[2];
    VkDescriptorSetLayout set_layout[2];
    VkDescriptorUpdate *descriptor_updates[2];
} Prog;
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int binding, uint32_t x, uint32_t y, uint32_t z) {
    (void)stage; (void)si; (void)x; (void)y; (void)z; resolves++;
    return (VkDescriptorBufferInfo){H(VkBuffer, 100 + (resource_change & DESCRIPTOR_UNIFORM)), uniform_offset + binding * 256, 16};
}
static bool vertex_debug_capturing(void) { return false; }
static VkDescriptorBufferInfo vertex_debug_info(void) { abort(); }
static VkDescriptorBufferInfo gmem_info(void) { resolves++; return (VkDescriptorBufferInfo){H(VkBuffer, 200 + (resource_change & DESCRIPTOR_STORAGE)), 32, 128}; }
static VkDescriptorBufferInfo gfast_info(void) { return gmem_info(); }
static VkDescriptorBufferInfo gmiss_info(void) { return gmem_info(); }
static VkSpan copy_dedupe(int slot, const void *p, uint64_t n, uint64_t align) {
    (void)p; (void)n; (void)align; return (VkSpan){.buf = H(VkBuffer, 300), .off = 4096 + slot * 256};
}
static VkSpan gmap_copy(void) { return (VkSpan){.buf = H(VkBuffer, 400), .off = 8192}; }
static VkDescriptorBufferInfo ubo_span(VkSpan span, uint64_t size) { return (VkDescriptorBufferInfo){span.buf, span.off, size}; }
static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    (void)stage; (void)si; (void)i; resolves++;
    *out = (VkDescriptorImageInfo){H(VkSampler, 500), H(VkImageView, 600 + (resource_change & DESCRIPTOR_TEXTURE)), VK_IMAGE_LAYOUT_GENERAL};
    sc[0] = sc[1] = 1; wh[0] = wh[1] = 16;
}
static VkDescriptorImageInfo storage_image_info(int stage, ShaderTexture *t) {
    (void)stage; (void)t; resolves++;
    return (VkDescriptorImageInfo){VK_NULL_HANDLE, H(VkImageView, 700 + (resource_change & DESCRIPTOR_IMAGE)), VK_IMAGE_LAYOUT_GENERAL};
}
VkSampler vk_default_sampler(bool linear) { (void)linear; return H(VkSampler, 500); }
VkImageView vk_dummy_view(int type) { (void)type; return H(VkImageView, 600); }
uint64_t vk_pending_seq(void) { return seq; }
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) { (void)layout; assert(++sets < 64); return H(VkDescriptorSet, sets); }
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
    (void)d; (void)a; creates++;
    if (fail_create) return VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET && ci->descriptorSetLayout);
    Template *t = calloc(1, sizeof *t); assert(t); t->count = ci->descriptorUpdateEntryCount;
    for (uint32_t i = 0; i < t->count; i++) {
        assert(ci->pDescriptorUpdateEntries[i].descriptorCount == 1 && ci->pDescriptorUpdateEntries[i].dstArrayElement == 0);
        t->entries[i] = ci->pDescriptorUpdateEntries[i];
    }
    *out = (VkDescriptorUpdateTemplate)t; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice d, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) { (void)d; (void)a; destroys++; free((void *)t); }
static void driver_write(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const void *value) {
    DriverSet *s = &stored[(uintptr_t)set]; assert(!s->sealed && binding < 96); s->written[binding] = true; s->types[binding] = type;
    if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) memcpy(&s->values[binding].image, value, sizeof(VkDescriptorImageInfo));
    else memcpy(&s->values[binding].buffer, value, sizeof(VkDescriptorBufferInfo));
}
static VKAPI_ATTR void VKAPI_CALL update_template(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate handle, const void *data) {
    (void)d; writes++; Template *t = (Template *)handle;
    for (unsigned i = 0; i < t->count; i++) { const VkDescriptorUpdateTemplateEntry *e = &t->entries[i]; driver_write(set, e->dstBinding, e->descriptorType, (const char *)data + e->offset); }
}
static VKAPI_ATTR void VKAPI_CALL update_legacy(VkDevice d, uint32_t count, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void)d; (void)c; assert(nc == 0); writes++;
    for (unsigned i = 0; i < count; i++) driver_write(w[i].dstSet, w[i].dstBinding, w[i].descriptorType, w[i].pImageInfo ? (const void *)w[i].pImageInfo : w[i].pBufferInfo);
}
#include "stage_set.inc"
static VkDescriptorSet run(Prog *p, ShaderInfo *si, uint32_t set, uint32_t *offsets) {
    uint32_t n = 0;
    VkDescriptorSet ds = stage_set(p, p->set_layout[set], set, set ? 1 : 0, si, NULL, NULL, 0, 0, 0, NULL, offsets, &n);
    assert(n == 5 && offsets[0] == uniform_offset + 256 && offsets[1] == uniform_offset + 768);
    assert(offsets[2] == 4096 + (set ? 0 : 256) && offsets[3] == 4096 + 512 && offsets[4] == 8192);
    stored[(uintptr_t)ds].sealed = true; // subsequent updates to any recorded set are invalid
    return ds;
}
static void configure(Prog *p, unsigned set) {
    p->nbinds = 8; p->set_layout[set] = H(VkDescriptorSetLayout, 10 + set);
    const VkBindingInfo b[] = {{set, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {set, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        {set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {set, 33, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        {set, 19, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {set, 20, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {set, 18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}};
    memcpy(p->binds, b, sizeof b); descriptor_plan(p->binds, p->nbinds, set, &p->descriptor_plans[set]);
}
int main(int argc, char **argv) {
    bool legacy = argc > 1 && !strcmp(argv[1], "legacy"), failed = argc > 1 && !strcmp(argv[1], "failure");
    bool allocation = argc > 1 && !strcmp(argv[1], "allocation"), no_cache = argc > 1 && !strcmp(argv[1], "no-cache");
    setenv("LARECOMP_VK_DESCRIPTOR_UPDATES", legacy ? "0" : "1", 1);
    if (argc > 1 && !strcmp(argv[1], "plan")) setenv("LARECOMP_VK_DESCRIPTOR_PLAN", "1", 1);
    vkCreateDescriptorUpdateTemplate = create; vkDestroyDescriptorUpdateTemplate = destroy; vkUpdateDescriptorSetWithTemplate = update_template; vkUpdateDescriptorSets = update_legacy;
    fail_create = failed; fail_allocate = allocation;
    if (no_cache) setenv("LARECOMP_NO_SET_CACHE", "1", 1);
    ShaderInfo si = {.ntex = 2}; si.tex[1].is_image = 1;
    Prog p = {0}; configure(&p, 0); uint32_t offsets[96];
    VkDescriptorSet first = run(&p, &si, 0, offsets); unsigned resolved_first = resolves;
    uniform_offset += 256; VkDescriptorSet same = run(&p, &si, 0, offsets);
    assert((no_cache ? same != first : same == first) && resolves == resolved_first * 2 && writes == (no_cache ? 2 : 1));
    // Each category independently changes the set, while previously recorded sets stay untouched.
    for (uint32_t c = 1; c <= 8; c <<= 1) {
        if (!legacy && !failed && !allocation) {
            VkDescriptorUpdate *u = p.descriptor_updates[0]; VkDescriptorValue changed[96];
            memcpy(changed, u->values, sizeof changed);
            if (c == DESCRIPTOR_UNIFORM) changed[0].buffer.range++;
            if (c == DESCRIPTOR_STORAGE) changed[2].buffer.offset++;
            if (c == DESCRIPTOR_TEXTURE) changed[1].image.sampler = H(VkSampler, 501);
            if (c == DESCRIPTOR_IMAGE) changed[4].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            assert(vk_descriptor_update_dirty(u, seq, changed) == c);
        }
        resource_change = c; VkDescriptorSet changed = run(&p, &si, 0, offsets); assert(changed != first);
        assert(stored[(uintptr_t)first].values[32].image.imageView == H(VkImageView, 600));
        resource_change = 0; first = run(&p, &si, 0, offsets);
    }
    seq++; VkDescriptorSet next = run(&p, &si, 0, offsets); assert(next != first); // identical values cannot reuse a retired pool
    Prog fragment = {0}; configure(&fragment, 1); VkDescriptorSet other = run(&fragment, &si, 1, offsets); assert(other != next);
    // Template and legacy writers must place independently expected payloads at the same bindings.
    DriverSet *s = &stored[(uintptr_t)next];
    const unsigned bindings[] = {1, 3, 4, 18, 19, 20, 32, 33};
    for (unsigned i = 0; i < 8; i++) assert(s->written[bindings[i]]);
    assert(s->values[1].buffer.offset == 0 && s->values[4].buffer.offset == 32 && s->values[4].buffer.range == 128);
    assert(s->values[33].image.imageView == H(VkImageView, 700));
    for (unsigned i = 0; i < 2; i++) { vk_descriptor_update_destroy(p.descriptor_updates[i], g_vk.dev); free(p.descriptor_updates[i]); vk_descriptor_update_destroy(fragment.descriptor_updates[i], g_vk.dev); free(fragment.descriptor_updates[i]); }
    assert(creates == (legacy || allocation ? 0 : 2) && destroys == (legacy || failed || allocation ? 0 : 2));
    fail_allocate = false;
    VkDescriptorUpdate bounds = {0}; uint32_t keys[96]; VkDescriptorType types[96];
    for (unsigned i = 0; i < 96; i++) { keys[i] = i; types[i] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; }
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 97, keys, types) == VK_ERROR_INITIALIZATION_FAILED);
    keys[1] = 0;
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 2, keys, types) == VK_ERROR_INITIALIZATION_FAILED);
    keys[1] = 1; types[0] = VK_DESCRIPTOR_TYPE_SAMPLER;
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 2, keys, types) == VK_ERROR_FEATURE_NOT_PRESENT);
    types[0] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    PFN_vkUpdateDescriptorSetWithTemplate saved = vkUpdateDescriptorSetWithTemplate; vkUpdateDescriptorSetWithTemplate = NULL;
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 2, keys, types) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkUpdateDescriptorSetWithTemplate = saved; fail_create = true;
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 96, keys, types) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(!bounds.ready && !bounds.update); vk_descriptor_update_destroy(&bounds, g_vk.dev);
    fail_create = false;
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 96, keys, types) == VK_SUCCESS);
    assert(bounds.count == 96 && bounds.categories == DESCRIPTOR_UNIFORM);
    vk_descriptor_update_destroy(&bounds, g_vk.dev); vk_descriptor_update_destroy(&bounds, g_vk.dev);
    assert(vk_descriptor_update_init(&bounds, g_vk.dev, H(VkDescriptorSetLayout, 10), 0, NULL, NULL) == VK_SUCCESS);
    vk_descriptor_update_write(&bounds, g_vk.dev, seq, H(VkDescriptorSet, 1), NULL);
    assert(!vk_descriptor_update_dirty(&bounds, seq, NULL)); vk_descriptor_update_destroy(&bounds, g_vk.dev);
    puts("PASS: actual stage_set writes, dynamic ordering, live-input checks on hits, four categories, immutable recorded sets, submission/fragment isolation and fallback");
}
