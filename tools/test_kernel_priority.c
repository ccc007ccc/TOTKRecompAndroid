// Exercise the production core-0 selector and safe-point handoffs with real
// threads. No guest binary, copied scheduler or timing-dependent ordering.
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include "../runtime/kernel.c"

volatile int g_fnprof = 1;
int g_game_version = TOTK_VERSION;
uint64_t g_guest_lo = 0x100000000ull;
uint64_t plat_now_ns(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;
}
void rt_fatal_message(const char *format,...) {
    va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);abort();
}
static unsigned count;
static uint64_t order[8];
static KThread *parent;
static void *worker(void *arg) {
    KThread *t=arg;t_cur=t;
    core0_acquire(t);
    pthread_mutex_lock(&c0_m);
    if(!g_rt_unwind && __atomic_load_n(&t->core,__ATOMIC_ACQUIRE)==0) {
        assert(c0_holder==t && t->core0_held && !parent->core0_held);
        assert(count<8);order[count++]=t->id;
    }
    pthread_mutex_unlock(&c0_m);
    core0_release(t);t_cur=NULL;return NULL;
}
static void queued(KThread *t) {
    uint64_t deadline=plat_now_ns()+2000000000ull;
    for(;;) {
        pthread_mutex_lock(&c0_m);bool found=false;
        for(KThread *p=c0_head;p;p=p->core0_next)found|=p==t;
        pthread_mutex_unlock(&c0_m);
        if(found)return;
        assert(plat_now_ns()<deadline);usleep(100);
    }
}
static pthread_t start(KThread *t) {
    pthread_t th;assert(!pthread_create(&th,NULL,worker,t));queued(t);return th;
}
static void priority(KThread *t,int value) {
    pthread_mutex_lock(&g_klock);thread_set_priority(t,value);pthread_mutex_unlock(&g_klock);
    assert(thread_priority(t)==value);
}
static void clean(void) {
    assert(!c0_head && !c0_tail && !c0_holder && !c0_priority_preempted);
    assert(c0_nevicted==0 && g_fnprof==1);count=0;
}
int main(int argc,char **argv) {
    alarm(20);
    bool off=argc>1 && !strcmp(argv[1],"off");
    bool other=argc>1 && !strcmp(argv[1],"other-game");
    if(other)g_game_version=182;
    unsetenv("LARECOMP_C0_SLICE_MS");unsetenv("LARECOMP_NO_CORE0");
    if(argc>1 && !strcmp(argv[1],"default"))unsetenv("LARECOMP_C0_PRIORITY");
    else setenv("LARECOMP_C0_PRIORITY",off?"0":"1",1);
    bool enabled=!other && (!TOTK_DIAGNOSTICS || !off);
    KThread owner={.id=80,.prio=10,.core=0,.core_mask=1,.started=true};
    parent=&owner;t_cur=&owner;

    // Priority order, stable arrival among equals, and legacy FIFO when off.
    for(unsigned round=0;round<20;++round) {
        KThread tasks[4]={{.id=1,.prio=56},{.id=2,.prio=44},{.id=3,.prio=55},{.id=4,.prio=44}};
        pthread_t threads[4];core0_acquire(&owner);
        for(unsigned i=0;i<4;++i)threads[i]=start(&tasks[i]);
        assert(!owner.core0_evicted && !count);
        core0_release(&owner);
        for(unsigned i=0;i<4;++i)pthread_join(threads[i],NULL);
        const uint64_t expected[4]={2,4,3,1};
        assert(count==4);
        for(unsigned i=0;i<4;++i)assert(order[i]==(enabled?expected[i]:i+1));
        clean();
    }

    // A higher-priority arrival cannot run until the owner acknowledges it.
    // The interrupted owner resumes ahead of its equal-priority queued peer.
    for(unsigned round=0;round<30;++round) {
        priority(&owner,56);core0_acquire(&owner);
        KThread peer={.id=1,.prio=56},high={.id=2,.prio=44};
        pthread_t a=start(&peer);assert(!owner.core0_evicted);
        pthread_t b=start(&high);
        assert(owner.core0_held && c0_holder==&owner && !count);
        assert(owner.core0_evicted==enabled);
        if(enabled) {
            assert(g_fnprof==3);kernel_preempt_point();
            assert(count==1 && order[0]==2 && owner.core0_held);
        }
        core0_release(&owner);pthread_join(a,NULL);pthread_join(b,NULL);
        assert(count==2 && order[0]==(enabled?2:1) && order[1]==(enabled?1:2));clean();
    }

    // A queued priority change joins the tail of its new priority group.
    priority(&owner,10);core0_acquire(&owner);
    KThread changed={.id=1,.prio=56},existing={.id=2,.prio=44};
    pthread_t a=start(&changed),b=start(&existing);
    priority(&changed,44);core0_release(&owner);pthread_join(a,NULL);pthread_join(b,NULL);
    assert(count==2 && order[0]==(enabled?2:1) && order[1]==(enabled?1:2));clean();

    // Raising a waiter or lowering the owner requests the same safe handoff.
    for(unsigned kind=0;kind<2;++kind) {
        priority(&owner,44);core0_acquire(&owner);
        KThread waiter={.id=1,.prio=55};a=start(&waiter);assert(!owner.core0_evicted);
        if(kind)priority(&owner,56);else priority(&waiter,40);
        assert(owner.core0_evicted==enabled && !count);
        if(enabled){kernel_preempt_point();assert(count==1 && owner.core0_held);}
        core0_release(&owner);pthread_join(a,NULL);assert(count==1);clean();
    }

    // An explicit release/yield still puts the owner behind equal-priority peers.
    priority(&owner,44);core0_acquire(&owner);
    KThread equal={.id=1,.prio=44};a=start(&equal);
    assert(!owner.core0_evicted);core0_release(&owner);core0_acquire(&owner);
    assert(count==1 && owner.core0_held);core0_release(&owner);pthread_join(a,NULL);clean();

    // A migrated waiter leaves without stealing core 0; stale preemption is harmless.
    priority(&owner,56);core0_acquire(&owner);
    KThread migrant={.id=1,.prio=40,.core_mask=1,.started=true};a=start(&migrant);
    pthread_mutex_lock(&g_klock);assert(thread_set_core_mask(&migrant,1,2)==R_OK);pthread_mutex_unlock(&g_klock);
    pthread_join(a,NULL);assert(!count && owner.core0_held);
    kernel_preempt_point();assert(owner.core0_held);core0_release(&owner);clean();

    // Reboot drains waiters and clears all pending ownership/priority requests.
    core0_acquire(&owner);KThread last={.id=1,.prio=40};a=start(&last);
    pthread_mutex_lock(&c0_m);g_rt_unwind=1;pthread_cond_broadcast(&c0_cv);pthread_mutex_unlock(&c0_m);
    pthread_join(a,NULL);core0_release(&owner);kernel_reset();clean();t_cur=NULL;
    printf("PASS: priority/FIFO selection, safe preemption, owner resumption, live priority changes, migration and reboot (enabled=%d)\n",enabled);
}
