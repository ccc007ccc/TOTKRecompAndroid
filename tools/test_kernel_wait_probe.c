// Real condition waits and address wakeups: diagnostic attribution must not
// leak across waits or threads, and kernel reset must discard old identities.
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/kernel.c"

uint64_t plat_now_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static _Atomic unsigned waiting;
static KThread target={.id=82};
static uint64_t first_waker,second_waker;
static bool long_trace;
static void *waiter(void *unused) {
    (void)unused;t_cur=&target;target.svc_now=0x35;target.svc_lr=0x1234;
    target.svc_args[0]=0x1000;
    pthread_mutex_lock(&g_klock);
    g_waitlist=&target;
    for(unsigned i=1;i<=2;++i) {
        target.wait_kind=WAIT_ARBITER;target.wait_addr=0x1000;
        memset(&wait_probe,0,sizeof wait_probe);
        waiting=i;
        assert(kwait(-1,NULL));
        assert(wait_probe.calls[0x34]==1 && wait_probe.ns[0x34]>0);
        assert(wait_probe.lr[0x34]==0x1234 && wait_probe.args[0x34][0]==0x1000);
        if(long_trace) {
            assert(wait_probe_long.end-wait_probe_long.start>=50000000ull);
            assert(wait_probe_long.thread==82 && wait_probe_long.frame==2000);
            assert(wait_probe_long.waker==(i==1?80:0));
        } else assert(!wait_probe_long.end);
        if(i==1) {
            first_waker=wait_probe.address_waker;
            assert(wait_probe.address_waker_lr==0x5678 && target.wait_kind==WAIT_NONE);
        } else second_waker=wait_probe.address_waker;
        pthread_mutex_unlock(&g_klock);
        wait_probe_long_flush();
        assert(!wait_probe_long.end && wait_probe_long.reports==(long_trace?i:0));
        pthread_mutex_lock(&g_klock);
    }
    g_waitlist=NULL;pthread_mutex_unlock(&g_klock);t_cur=NULL;return NULL;
}
int main(int argc,char **argv) {
    (void)argv;long_trace=argc==1;
    alarm(10);setenv("LARECOMP_LOG_KERNEL_WAITS","1",1);
    if(long_trace)setenv("LARECOMP_KERNEL_WAIT_TRACE_FRAME","2000",1);
    else unsetenv("LARECOMP_KERNEL_WAIT_TRACE_FRAME");
    FILE *log=tmpfile();assert(log);int original=dup(STDERR_FILENO);assert(original>=0);
    assert(dup2(fileno(log),STDERR_FILENO)==STDERR_FILENO);
    pthread_cond_init(&target.cond,NULL);
    KThread producer={.id=80,.svc_lr=0x5678};t_cur=&producer;
    atomic_store(&wait_probe_presenter,82);
    atomic_store(&wait_probe_frame,2000);
    pthread_t thread;assert(!pthread_create(&thread,NULL,waiter,NULL));
    while(waiting<1)usleep(100);
    usleep(60000);
    pthread_mutex_lock(&g_klock);
    assert(arbiter_wake(0x1000,1)==1);
    pthread_mutex_unlock(&g_klock);
    while(waiting<2)usleep(100);
    usleep(60000);
    pthread_mutex_lock(&g_klock);
    pthread_cond_signal(&target.cond); // unrelated/spurious wake has no address signaler
    pthread_mutex_unlock(&g_klock);
    pthread_join(thread,NULL);
    assert(first_waker==80 && second_waker==0);
    assert(!wait_probe.calls[0x34]); // producer never borrowed the waiter's TLS
    assert(atomic_load(&wait_probe_signaler)==80);
    fflush(stderr);rewind(log);char output[2048]={0};size_t count=fread(output,1,sizeof output-1,log);
    if(long_trace) {
        assert(strstr(output,"thread 82 svc-34 ns ") && strstr(output,"lr 1234 arg0 1000 waker 80 lr 5678"));
        assert(strstr(output,"lr 1234 arg0 1000 waker 0 lr 0"));
    } else assert(!count);
    assert(dup2(original,STDERR_FILENO)==STDERR_FILENO);close(original);fclose(log);
    // Bounds and filtering use independent timestamps, with no real sleep.
    producer.svc_now=0x35;
    atomic_store(&wait_probe_frame,1999);wait_probe_long_record(100,60000100);
    assert(!wait_probe_long.end);
    atomic_store(&wait_probe_frame,2000);wait_probe_long_record(100,49000100);
    assert(!wait_probe_long.end);
    wait_probe_long_record(100,60000100);
    if(long_trace) {
        assert(wait_probe_long.end==60000100);
        wait_probe_long_record(200,55000200);assert(wait_probe_long.end==60000100);
        wait_probe_long.start=wait_probe_long.end=0;wait_probe_long.reports=128;
        wait_probe_long_record(100,70000100);assert(!wait_probe_long.end);
    } else assert(!wait_probe_long.end);
    atomic_store(&wait_probe_worker,126);
    kernel_reset();
    assert(!atomic_load(&wait_probe_presenter) && !atomic_load(&wait_probe_signaler));
    assert(!atomic_load(&wait_probe_worker));
    assert(!wait_probe_long.end && !wait_probe_long.reports);
    for(unsigned i=0;i<256;++i)assert(!wait_probe_wakers[i].target);
    pthread_cond_destroy(&target.cond);t_cur=NULL;
    puts("PASS: actual address wake attribution, longest wait timestamps, filter/budget, disabled trace, TLS and reset");
}
