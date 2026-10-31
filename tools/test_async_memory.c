// Run the actual Android request queue on the host. Vulkan/guest execution are
// doubles; verify CPU flush ordering, a 64-bit range, and render-thread reentry.
#define VK_NO_PROTOTYPES
#define TOTK_DIAGNOSTICS 1 // fixture supplies diagnostic hooks explicitly
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/async.c"

VkG g_vk;
typedef struct { unsigned kind; uint64_t a, b; } Observation;
static Observation seen[16];
static unsigned observations;
static bool nested;
static void observe(unsigned kind, uint64_t a, uint64_t b) {
    assert(observations < 16); seen[observations++] = (Observation){kind, a, b};
}
static void nested_call(void *arg) { uint64_t *value = arg; observe(4, *value, 0); *value = 42; }
static void synchronous_call(void *arg) {
    uint64_t *value = arg; observe(3, *value, 0); vk_async_call_sync(nested_call, arg);
}
static VkResult idle(VkDevice device) { (void)device; return VK_SUCCESS; }
#ifndef TOTK_VK_COMMAND_WORKER
PFN_vkDeviceWaitIdle vkDeviceWaitIdle = idle;
#else
static bool worker_test;
static pthread_t test_producer;
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static bool start_released;
static _Atomic bool start_race;
static unsigned driver_draws;
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (atomic_load(&start_race) && pthread_equal(pthread_self(), test_producer)) {
        pthread_mutex_lock(&start_lock);
        start_released = true; pthread_cond_broadcast(&start_cond);
        pthread_mutex_unlock(&start_lock);
    }
    return __real_pthread_cond_wait(c, m);
}
static VKAPI_ATTR void VKAPI_CALL driver_draw(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t base) {
    (void)cb; (void)count; (void)inst; (void)base; assert(first == driver_draws++);
}
static VKAPI_ATTR void VKAPI_CALL driver_pipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) { (void)cb; (void)bp; (void)p; }
static VKAPI_ATTR void VKAPI_CALL driver_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
    uint32_t f, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    (void)cb; (void)bp; (void)l; (void)f; (void)n; (void)s; (void)nd; (void)d;
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_end(VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
static void worker_exec(void) {
    if (!vk_command_worker_active()) {
        pthread_mutex_lock(&start_lock);
        while (!start_released) pthread_cond_wait(&start_cond, &start_lock);
        pthread_mutex_unlock(&start_lock);
        assert(vk_command_worker_start(false));
    }
    vkCmdDraw((VkCommandBuffer)(uintptr_t)1, 3, 1, 0, 0); // remains in an unpublished partial slot
}
static void worker_nested(void *unused) {
    (void)unused; assert(driver_draws == 2);
    vkCmdDraw((VkCommandBuffer)(uintptr_t)1, 3, 1, 2, 0);
}
static void worker_callback(void *unused) {
    (void)unused; assert(driver_draws == 1); // queued callback drains preceding EXEC
    vkCmdDraw((VkCommandBuffer)(uintptr_t)1, 3, 1, 1, 0);
    vk_async_call_sync(worker_nested, NULL); // in-place reentry drains before and after
    assert(driver_draws == 3);
    vkCmdDraw((VkCommandBuffer)(uintptr_t)1, 3, 1, 3, 0);
    vk_async_drain(); assert(driver_draws == 4); // render-thread drain must publish its own bytes
    vkCmdDraw((VkCommandBuffer)(uintptr_t)1, 3, 1, 4, 0); // outer callback also drains on return
}
static void worker_stop(void *unused) { (void)unused; vk_command_worker_stop(); }
#endif
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
bool vk_timing_trace_on(void) { return false; }
void vk_timing_trace_wait(int kind, uint64_t ns) { (void)kind; (void)ns; }
void test_session_frame(uint64_t frame, bool scene, void (*idle_fn)(void)) { (void)frame; (void)scene; (void)idle_fn; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t vk_test_scene_frame(void) { return 0; }
void vk_execute_now(uint64_t handle) {
#ifdef TOTK_VK_COMMAND_WORKER
    if (worker_test) { worker_exec(); return; }
#endif
    observe(1, ((const CmdRec *)(uintptr_t)handle)->x[0], 0);
}
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
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool entered, released;
static void held_call(void *arg) {
    pthread_mutex_lock(&gate_lock);
    entered = true; pthread_cond_broadcast(&gate_cond);
    while (!released) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    vk_async_call(nested_call, arg); // render-thread reentry must run in place
}
static void test_render_trace(void) {
    FILE *log = tmpfile(); assert(log);
    int original = dup(STDERR_FILENO); assert(original >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) == STDERR_FILENO);
    RenderFrameTrace trace = {.enabled = true, .first_frame = 2};
    render_trace_request(&trace, REQ_PRESENT, 2000000, 1000000000, 1, 5000000);
    assert(!trace.reports && !trace.requests);
    render_trace_request(&trace, REQ_EXEC, 3000000, 0, 0, 0);
    render_trace_request(&trace, REQ_CALL, 5000000, 0, 0, 0);
    render_trace_request(&trace, REQ_PRESENT, 4000000, 1080000000, 2, 12000000);
    assert(trace.reports == 1 && !trace.requests);
    for (unsigned i = 0; i < REQ_COUNT; ++i) assert(!trace.ns[i]);
    fflush(stderr); rewind(log);
    char line[1024]; assert(fgets(line, sizeof line, log));
    assert(strstr(line, "frame 2: wall 80.000 work 12.000 gap 68.000 cpu 7.000 ms"));
    assert(strstr(line, "exec 3.000 present 4.000 fence 0.000 write 0.000 call 5.000 driver 0.000; requests 3"));
    assert(strstr(line, "; ns 1000000000..1080000000"));
    assert(fgetc(log) == EOF);
    // The first-frame filter still advances the observation boundary.
    trace.first_frame = 4;
    render_trace_request(&trace, REQ_PRESENT, 1000000, 1180000000, 3, 15000000);
    assert(trace.reports == 1 && trace.previous_frame == 3);
    trace.reports = 128;
    render_trace_request(&trace, REQ_PRESENT, 1000000, 1280000000, 4, 16000000);
    assert(trace.reports == 128 && trace.previous_ns == 1280000000);
    vk_async_trace_reset(); // real backend reset, with a continuing frame counter
    render_trace_request(&trace, REQ_PRESENT, 1000000, 1380000000, 5, 17000000);
    assert(trace.reports == 0 && trace.previous_frame == 5);
    trace.enabled = false;
    render_trace_request(&trace, REQ_EXEC, 1000000, 0, 0, 0);
    assert(!trace.requests && !trace.ns[REQ_EXEC]);
    assert(dup2(original, STDERR_FILENO) == STDERR_FILENO); close(original); fclose(log);
    puts("PASS: render-frame request attribution, completed frame number, bounded logs, filter, reboot and disabled tracing");
}
int main(void) {
    test_render_trace();
#ifdef TOTK_VK_COMMAND_WORKER
    vkDeviceWaitIdle = idle; vkCmdDraw = driver_draw; vkCmdBindPipeline = driver_pipeline;
    vkCmdBindDescriptorSets = driver_bind; vkEndCommandBuffer = driver_end;
    test_producer = pthread_self();
#endif
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        setenv("LARECOMP_VK_ASYNC", "0", 1);
        vk_async_memory_written(123, 456);
        assert(observations == 1 && seen[0].a == 123 && seen[0].b == 456 && !started);
        uint64_t value = 7;
        vk_async_call(nested_call, &value);
        assert(value == 42 && observations == 2 && seen[1].kind == 4 && !started);
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
    value = 23;
    vk_async_call(held_call, &value);
    pthread_mutex_lock(&gate_lock);
    while (!entered) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    // Returning while the render worker is held proves this is nonblocking.
    // The following EXEC must stay after that update, even under queue activity.
    vk_async_execute((uintptr_t)&first);
    assert(value == 23 && observations == 6);
    pthread_mutex_lock(&gate_lock); released = true; pthread_cond_broadcast(&gate_cond); pthread_mutex_unlock(&gate_lock);
    vk_async_drain();
    assert(value == 42 && observations == 8 && seen[6].kind == 4 && seen[6].a == 23 && seen[7].kind == 1);
    puts("PASS: CPU flush/callback order, nonblocking enqueue, render-thread reentry, 64-bit ranges and synchronous mode");
#ifdef TOTK_VK_COMMAND_WORKER
    worker_test = true;
    atomic_store(&start_race, true);
    vk_async_execute((uintptr_t)&first);
    vk_async_drain(); // starts waiting while worker is inactive; that EXEC starts it later
    atomic_store(&start_race, false);
    assert(driver_draws == 1 && vk_command_worker_stats().commands == 1);
    vk_async_call_sync(worker_callback, NULL);
    assert(driver_draws == 5 && vk_command_worker_stats().commands == 5);
    vk_async_call_sync(worker_stop, NULL); assert(!vk_command_worker_active());
    puts("PASS: actual request queue drains a newly started worker, callback and render-thread handoffs, nested reentry and stop");
#endif
}
