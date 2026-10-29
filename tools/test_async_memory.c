// Run the actual Android request queue on the host. Vulkan/guest execution are
// doubles; verify CPU flush ordering, a 64-bit range, and render-thread reentry.
#define VK_NO_PROTOTYPES
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/async.c"

VkG g_vk;
typedef struct { unsigned kind; uint64_t a, b; } Observation;
static Observation seen[8];
static unsigned observations;
static bool nested;
static void observe(unsigned kind, uint64_t a, uint64_t b) {
    assert(observations < 8); seen[observations++] = (Observation){kind, a, b};
}
static void nested_call(void *arg) { uint64_t *value = arg; observe(4, *value, 0); *value = 42; }
static void synchronous_call(void *arg) {
    uint64_t *value = arg; observe(3, *value, 0); vk_async_call_sync(nested_call, arg);
}
static VkResult idle(VkDevice device) { (void)device; return VK_SUCCESS; }
PFN_vkDeviceWaitIdle vkDeviceWaitIdle = idle;
void rt_fatal_message(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); abort();
}
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
void plat_thread_unpin(void) {}
void plat_render_work(uint64_t work, uint64_t target) { (void)work; (void)target; }
void kernel_blocking_begin(void) {}
void kernel_blocking_end(void) {}
bool framepace_on(void) { return false; }
uint64_t framepace_period_ns(void) { return 16666667; }
bool test_session_enabled(void) { return false; }
void test_session_frame(uint64_t frame, bool scene, void (*idle_fn)(void)) { (void)frame; (void)scene; (void)idle_fn; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t vk_test_scene_frame(void) { return 0; }
void vk_execute_now(uint64_t handle) { observe(1, ((const CmdRec *)(uintptr_t)handle)->x[0], 0); }
void vk_present_now(NvnTexture *texture) { (void)texture; abort(); }
void nvn_sync_set_seq(uint64_t sync, uint64_t seq) { (void)sync; (void)seq; abort(); }
uint64_t renderer_fence_seq(void) { return 0; }
uint64_t vk_submit_retired_seq(void) { return 0; }
void vk_memory_written(uint64_t addr, uint64_t size) {
    observe(2, addr, size);
    if (nested && vk_async_is_render_thread()) { nested = false; vk_async_memory_written(addr + 1, 4); }
}
SubmitSnapshot *submit_snapshot_capture(const void *list, size_t bytes, uint64_t retired, SubmitRead read, bool *sync) {
    (void)list; (void)bytes; (void)retired; (void)read; *sync = false; return NULL;
}
void submit_snapshot_begin(SubmitSnapshot *snapshot) { (void)snapshot; }
void submit_snapshot_end(void) {}
void submit_snapshot_free(SubmitSnapshot *snapshot) { (void)snapshot; }
bool nvn_submit_read(uint64_t addr, uint64_t size, void *dst, uint64_t *lo, uint64_t *hi) {
    (void)addr; (void)size; (void)dst; (void)lo; (void)hi; abort();
}
int main(void) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        setenv("LARECOMP_VK_ASYNC", "0", 1);
        vk_async_memory_written(123, 456);
        assert(observations == 1 && seen[0].a == 123 && seen[0].b == 456 && !started);
        _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    setenv("LARECOMP_NO_LIST_COPY", "1", 1);
    setenv("LARECOMP_VK_ASYNC", "1", 1);
    CmdRec first = {.op = CMD_END, .x = {11}}, second = {.op = CMD_END, .x = {22}};
    nested = true;
    vk_async_execute((uintptr_t)&first);
    vk_async_memory_written(0x300001234ull, (1ull << 33) + 13);
    vk_async_execute((uintptr_t)&second);
    uint64_t value = 17;
    vk_async_call_sync(synchronous_call, &value);
    assert(value == 42);
    vk_async_drain();
    assert(observations == 6);
    assert(seen[0].kind == 1 && seen[0].a == 11);
    assert(seen[1].kind == 2 && seen[1].a == 0x300001234ull && seen[1].b == (1ull << 33) + 13);
    assert(seen[2].kind == 2 && seen[2].a == 0x300001235ull && seen[2].b == 4);
    assert(seen[3].kind == 1 && seen[3].a == 22);
    assert(seen[4].kind == 3 && seen[4].a == 17 && seen[5].kind == 4 && seen[5].a == 17);
    puts("PASS: CPU flush ordered between command lists, 64-bit range retained, render-thread reentry and synchronous mode");
}
