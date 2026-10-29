// Production NVN registration with a pending reader observing the old entry.
#include <assert.h>
#include "../runtime/nvn/objects.c"

static unsigned drains;
static NvnTexEntry *watched;
static NvnTexEntry before_drain;
static bool reuse;
static bool queue_mode;
static struct { void (*call)(void *); void *arg; } updates[16];
static unsigned head, tail;
bool vk_async_on(void) { return queue_mode; }
bool vk_deferred_resources_default(void) { return true; }
void vk_async_call(void (*call)(void *), void *arg) {
    assert(queue_mode && tail < 16);
    updates[tail].call = call; updates[tail++].arg = arg;
}
static void run_update(void) {
    assert(head < tail);
    // Regression for registry -> queue -> registry deadlock: publication must
    // finish even while the game thread holds the object registry lock.
    pthread_mutex_lock(&g_live_lock);
    updates[head].call(updates[head].arg); ++head;
    pthread_mutex_unlock(&g_live_lock);
}
uint64_t renderer_present_count(void) { return 1; }
void backend_texture_destroyed(NvnTexture *t) { (void)t; assert(!"owner still holds texture"); }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; abort(); }
bool vk_timing_trace_on(void) { return false; }
void vk_timing_trace_wait(int kind, uint64_t ns) { (void)kind; (void)ns; }
void vk_async_drain(void) {
    ++drains;
    // No descriptor write may be published before queued readers finish.
    assert(watched && !memcmp(watched, &before_drain, sizeof before_drain));
    while (head < tail) run_update();
}

static void queued_registration(void) {
    NvnTexture a = {.refs = 2}, b = {.refs = 1}; // a already has one descriptor
    uint64_t ao[24] = {(uintptr_t)&a}, bo[24] = {(uintptr_t)&b};
    NvnTexEntry entry = {.tex = &a};
    NvnDescPool pool = {.count = 1, .entries = &entry};
    uint64_t object = (uintptr_t)&pool;
    Cpu c = {0}; c.x[0] = (uintptr_t)&object; c.x[2] = (uintptr_t)ao;
    NvnTextureView view = {.setmask = 1, .base_level = 2, .num_levels = 1};
    c.x[3] = (uintptr_t)&view;
    register_texture(&c);
    assert(!drains && !entry.has_view && a.refs == 3); // old queued draw still sees old slot
    view.base_level = 7; // caller's stack can change after registration returns
    c.x[2] = (uintptr_t)bo; c.x[3] = 0;
    register_texture(&c); register_texture(&c);
    assert(!drains && entry.tex == &a && b.refs == 3);
    bo[0] = 0; --b.refs; // release guest owner; both pending updates retain b
    run_update();
    nvn_texture_updates_collect();
    assert(entry.tex == &a && entry.has_view && entry.view.base_level == 2 && a.refs == 2);
    run_update();
    nvn_texture_updates_collect();
    assert(entry.tex == &b && !entry.has_view && b.refs == 2);
    NvnTexEntry saved = entry;
    run_update();
    nvn_texture_updates_collect();
    assert(!memcmp(&saved, &entry, sizeof entry) && b.refs == 1); // duplicate drops only its reserved ref
    assert(g_nretired == 1 && g_retired[0].t == &a);
    c.x[2] = (uintptr_t)ao; c.x[3] = (uintptr_t)&view;
    register_texture(&c); // pool teardown must finish this still-pending publication
    assert(head < tail && entry.tex == &b && a.refs == 3);
    watched = &entry; before_drain = entry;
    hle_nvnTexturePoolFinalize(&c);
    assert(drains == 1 && !object && !entry.tex && head == tail);
    assert(g_nretired == 3 && g_retired[1].t == &b && g_retired[2].t == &a && b.refs == 1);
    assert(!atomic_load_explicit(&texture_updates_done, memory_order_relaxed));
    free(g_retired); g_retired = NULL; g_nretired = g_cap_retired = 0;
    puts("PASS: queued descriptors preserve old readers, capture views, pin finalized objects, transfer refs and drain before pool teardown");
}

static void registration(Cpu *c, bool changed) {
    before_drain = *watched;
    unsigned n = drains;
    register_texture(c);
    assert(drains == n + (before_drain.tex && (changed || !reuse)));
    if (!changed && reuse) assert(!memcmp(watched, &before_drain, sizeof before_drain));
}

int main(int argc, char **argv) {
    reuse = argc < 2 || strcmp(argv[1], "off");
    queue_mode = argc > 1 && !strncmp(argv[1], "queued", 6);
    if (argc > 1 && !strcmp(argv[1], "queued-default")) unsetenv("LARECOMP_TEXTURE_QUEUE");
    else setenv("LARECOMP_TEXTURE_QUEUE", queue_mode ? "1" : "0", 1);
    if (queue_mode) { queued_registration(); return 0; }
    if (!reuse) setenv("LARECOMP_TEXTURE_REUSE", "0", 1);
    else unsetenv("LARECOMP_TEXTURE_REUSE"); // exercise the production default
    NvnTexture a = {.refs = 1}, b = {.refs = 1};
    uint64_t ao[24] = {(uintptr_t)&a}, bo[24] = {(uintptr_t)&b}, alias[24] = {(uintptr_t)&a};
    G(uint32_t, alias, 8) = OBJ_TAG;
    const uint8_t swizzle[4] = {2, 3, 4, 1};
    memcpy((uint8_t *)alias + 12, swizzle, 4);
    NvnTexEntry entry = {0};
    NvnDescPool pool = {.count = 1, .entries = &entry};
    uint64_t pool_object = (uintptr_t)&pool;
    Cpu c = {0}; c.x[0] = (uintptr_t)&pool_object; c.x[2] = (uintptr_t)ao;
    watched = &entry;
    registration(&c, true); assert(entry.tex == &a && a.refs == 2);
    for (unsigned i = 0; i < 1000; ++i) registration(&c, false);
    assert(a.refs == 2);
    a.cpu_write_epoch = 9; // descriptor equality must not erase separate texel invalidation
    registration(&c, false); assert(a.cpu_write_epoch == 9);

    NvnTextureView view = {.setmask = 1, .base_level = 2, .num_levels = 1};
    c.x[3] = (uintptr_t)&view;
    registration(&c, true); assert(entry.view.base_level == 2);
    NvnTextureView copy = view;
    c.x[3] = (uintptr_t)&copy;
    registration(&c, false); // equal payload at a different address
    copy.base_level = 3;
    registration(&c, true); // same pointer, different payload
    copy.setmask |= 2; copy.min_layer = 2; copy.num_layers = 1;
    registration(&c, true);
    copy.setmask |= 4; copy.format = 0x25;
    registration(&c, true);
    copy.setmask |= 16; copy.ds_mode = 1;
    registration(&c, true);
    copy.setmask |= 32; copy.target = 1;
    registration(&c, true);
    c.x[3] = 0;
    registration(&c, true); assert(!entry.has_view);
    registration(&c, false);

    // Shared storage can have a different object swizzle (HUD/minimap alpha).
    c.x[2] = (uintptr_t)alias;
    registration(&c, true); assert(entry.has_view && entry.view.setmask == 8);
    assert(!memcmp(entry.view.swizzle, swizzle, 4));
    registration(&c, false); assert(a.refs == 2);
    view = (NvnTextureView){.setmask = 8, .swizzle = {2, 3, 4, 1}};
    c.x[3] = (uintptr_t)&view;
    registration(&c, false); // explicit and synthesized swizzles agree
    c.x[2] = (uintptr_t)ao;
    registration(&c, false); // explicit view overrides the object's swizzle
    view.swizzle[3] = 5;
    registration(&c, true);
    c.x[2] = (uintptr_t)bo;
    registration(&c, true); assert(a.refs == 2 && b.refs == 2 && entry.tex == &b);
    assert(g_nretired == 1 && g_retired[0].t == &a); // old reference follows the existing deferred release
    registration(&c, false);
    c.x[1] = 1; unsigned n = drains;
    register_texture(&c); assert(drains == n && b.refs == 2); // out-of-range slot
    free(g_retired); g_retired = NULL; g_nretired = g_cap_retired = 0;
    puts(reuse ? "PASS: identical descriptors skip writes/waits; changed views/swizzles/textures drain before mutation, refs retained" :
                 "PASS: comparison switch retains original drain behavior");
}
