// Android regression test of the real core-0 queue and affinity helper; no game files needed.
// Build with -ffunction-sections -fdata-sections -Wl,--gc-sections so unused kernel services are discarded.
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/kernel.c"

volatile int g_fnprof = 1;  // profiling must survive scheduler requests
int g_game_version = TOTK_VERSION;
uint64_t g_guest_lo = 0x100000000ull;
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static atomic_int ran;
static void *worker(void *arg) {
    KThread *t = arg;
    t_cur = t;
    core0_acquire(t);
    kernel_preempt_point();
    atomic_fetch_add(&ran, 1);
    core0_release(t);
    return NULL;
}
static void wait_queued(KThread *t) {
    uint64_t end = plat_now_ns() + 2000000000ull;
    for (;;) {
        pthread_mutex_lock(&c0_m);
        bool found = false;
        for (KThread *p = c0_head; p; p = p->core0_next) found |= p == t;
        pthread_mutex_unlock(&c0_m);
        if (found) return;
        assert(plat_now_ns() < end);
        usleep(100);
    }
}
static uint32_t set_mask(KThread *t, int ideal, uint64_t mask) {
    pthread_mutex_lock(&g_klock);
    uint32_t r = thread_set_core_mask(t, ideal, mask);
    pthread_mutex_unlock(&g_klock);
    return r;
}

int main(int argc,char **argv) {
    alarm(15);
    if(argc==2 && !strcmp(argv[1],"--trace-hold")) {
        unsetenv("LARECOMP_C0_SLICE_MS");
        setenv("LARECOMP_LOG_KERNEL_WAITS","1",1);
        setenv("LARECOMP_KERNEL_WAIT_TRACE_FRAME","2000",1);
#if TOTK_DIAGNOSTICS
        atomic_store(&wait_probe_frame,2000);
#endif
        FILE *log=tmpfile();assert(log);int saved=dup(STDERR_FILENO);assert(saved>=0);
        assert(dup2(fileno(log),STDERR_FILENO)==STDERR_FILENO);
        KThread owner={.id=80,.core=0,.core_mask=1},waiter={.id=128,.core=0,.core_mask=1};
        t_cur=&owner;core0_acquire(&owner);
        pthread_t th;assert(!pthread_create(&th,NULL,worker,&waiter));wait_queued(&waiter);
        usleep(15000);core0_release(&owner);pthread_join(th,NULL);
        fflush(stderr);rewind(log);char text[1024]={0};size_t length=fread(text,1,sizeof text-1,log);
#if TOTK_DIAGNOSTICS
        assert(strstr(text,"[kernel-core0-hold] frame 2000 holder 80 next 128 ns "));
        assert(c0_hold_reports==1);
#else
        assert(!strstr(text,"[kernel-core0-hold]"));
#endif
        (void)length;
        assert(dup2(saved,STDERR_FILENO)==STDERR_FILENO);close(saved);fclose(log);
        kernel_reset();
#if TOTK_DIAGNOSTICS
        assert(!c0_hold_reports);
#endif
        t_cur=NULL;puts("PASS: real queued core-0 holder trace, frame/owner identity, release isolation and reset");return 0;
    }
    if(argc>1) {
        assert(argc==2 && (!strcmp(argv[1],"--disabled") || !strcmp(argv[1],"--no-core0") ||
                          !strcmp(argv[1],"--default") || !strcmp(argv[1],"--other-game")));
        unsetenv("LARECOMP_C0_GUEST_YIELD");
        if(!strcmp(argv[1],"--disabled"))setenv("LARECOMP_C0_GUEST_YIELD","0",1);
        if(!strcmp(argv[1],"--no-core0"))setenv("LARECOMP_NO_CORE0","1",1);
        if(!strcmp(argv[1],"--other-game"))g_game_version=182;
        KThread sole={.core=0,.ideal_core=0,.core_mask=1};t_cur=&sole;
        core0_acquire(&sole);
        bool expected=!strcmp(argv[1],"--default") || (!TOTK_DIAGNOSTICS && !strcmp(argv[1],"--disabled"));
        assert(core0_guest_yield(0)==expected && core0_guest_yield(-1)==expected);
        core0_release(&sole);
        printf("PASS: yield policy %s (diagnostics=%d)\n",argv[1],TOTK_DIAGNOSTICS);return 0;
    }
    setenv("LARECOMP_C0_SLICE_MS", "1", 1);
    KThread owner = {.core = 0, .ideal_core = 0, .core_mask = 1, .started = true};
    KThread t = {.core = 0, .ideal_core = 0, .core_mask = 1};
    assert(set_mask(&t, -1, 2) == R_OK && t.core == 1 && t.ideal_core == -1 && t.core_mask == 2);
    assert(set_mask(&t, 2, 4) == R_OK && t.core == 2);
    assert(set_mask(&t, -3, 6) == R_OK && t.ideal_core == 2 && t.core_mask == 6);
    assert(set_mask(&t, 0, 2) == R_INVALID_COMBINATION && t.core_mask == 6);
    assert(set_mask(&t, -1, 0) == R_INVALID_COMBINATION && t.core_mask == 6);
    assert(set_mask(&t, -4, 1) == R_INVALID_CORE);
    assert(set_mask(&t, -1, 16) == R_INVALID_CORE);
    assert(set_mask(&t, -2, 0) == R_OK && t.core == 0 && t.ideal_core == 0 && t.core_mask == 1);

    for (int i = 0; i < 100; i++) {
        t = (KThread){.core = 0, .ideal_core = 0, .core_mask = 1, .started = true};
        t_cur = &owner;
        core0_acquire(&owner);
        atomic_store(&ran, 0);
        pthread_t th;
        assert(!pthread_create(&th, NULL, worker, &t));
        wait_queued(&t);
        // A waiter reassigned to core 1 must escape even while core 0's owner never yields.
        assert(set_mask(&t, -1, 2) == R_OK);
        pthread_join(th, NULL);
        assert(atomic_load(&ran) == 1 && owner.core0_held && c0_holder == &owner);
        core0_release(&owner);
        assert(!c0_head && !c0_tail && !c0_holder && !(g_fnprof & 2));
    }

    t = (KThread){.core = 0, .ideal_core = 0, .core_mask = 1, .started = true};
    core0_acquire(&owner);
    atomic_store(&ran, 0);
    pthread_t th;
    assert(!pthread_create(&th, NULL, worker, &t));
    wait_queued(&t);
    usleep(20000);  // the slicer has requested preemption, but the old owner hasn't acknowledged it
    assert(atomic_load(&ran) == 0 && c0_holder == &owner && owner.core0_evicted);
    kernel_preempt_point();
    pthread_join(th, NULL);
    assert(atomic_load(&ran) == 1 && owner.core0_held);
    core0_release(&owner);
    assert(g_fnprof == 1 && c0_nevicted == 0);

    setenv("LARECOMP_C0_GUEST_YIELD","1",1);
    // The SVC release/reacquire must hand ownership to an already queued peer
    // before returning, even with no intervening host sched_yield.
    for(int kind=0;kind<2;++kind)for(int i=0;i<20;++i) {
        core0_acquire(&owner);atomic_store(&ran,0);
        assert(!pthread_create(&th,NULL,worker,&t));wait_queued(&t);
        core0_release(&owner);
        assert(core0_guest_yield(kind?-1:0));
        core0_acquire(&owner);
        assert(atomic_load(&ran)==1 && c0_holder==&owner);
        pthread_join(th,NULL);core0_release(&owner);
    }
    assert(!core0_guest_yield(-2) && !core0_guest_yield(1));
    assert(set_mask(&owner,-1,3)==R_OK && !core0_guest_yield(-1)); // migration remains eligible
    assert(set_mask(&owner,1,2)==R_OK && !core0_guest_yield(-1)); // another guest core
    assert(set_mask(&owner,0,1)==R_OK && core0_guest_yield(-1));

    // A reboot must let waiters leave the queue even if an owner is still unwinding.
    core0_acquire(&owner);
    atomic_store(&ran, 0);
    assert(!pthread_create(&th, NULL, worker, &t));
    wait_queued(&t);
    pthread_mutex_lock(&c0_m);
    g_rt_unwind = 1;
    pthread_cond_broadcast(&c0_cv);
    pthread_mutex_unlock(&c0_m);
    pthread_join(th, NULL);
    core0_release(&owner);
    assert(!c0_head && !c0_tail && c0_nevicted == 0);
    puts("PASS: affinity validation, 100 queued migrations, 40 FIFO yields, safe handoff, reboot wakeup");
}
