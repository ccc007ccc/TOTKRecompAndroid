// A high-priority mutex waiter must let its low-priority owner run ahead of
// unrelated medium-priority work. Uses the real arbiter and core-0 queue.
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/wait.h>
#include "../runtime/kernel.c"

volatile int g_fnprof=1;
int g_game_version=TOTK_VERSION;
uint64_t g_guest_lo=0x100000000ull;
uint64_t plat_now_ns(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;
}
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *format,...) {
    va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);abort();
}
static _Atomic unsigned medium_ran;
static uint32_t mutex_word;
static bool inheritance_enabled, fail_realloc;
void *__real_realloc(void *pointer,size_t size);
void *__wrap_realloc(void *pointer,size_t size) {
    return fail_realloc?NULL:__real_realloc(pointer,size);
}
static KThread *new_thread(int priority) {
    KThread *t=thread_new(0,0,0,priority,0);
    t->handle=kernel_new_handle(&t->obj);t->started=true;return t;
}
static void *medium_main(void *arg) {
    KThread *t=arg;t_cur=t;
    for(unsigned i=0;i<100;++i) {
        core0_acquire(t);atomic_fetch_add(&medium_ran,1);core0_release(t);
    }
    t_cur=NULL;return NULL;
}
static void *high_main(void *arg) {
    KThread *t=arg;t_cur=t;
    uint32_t owner=mutex_word&~WAITERS_BIT;
    assert(svc_arbitrate_lock(owner,(uintptr_t)&mutex_word,t->handle)==R_OK);
    core0_acquire(t);
    assert((mutex_word&~WAITERS_BIT)==t->handle);
    assert(svc_arbitrate_unlock((uintptr_t)&mutex_word)==R_OK);
    core0_release(t);t_cur=NULL;return NULL;
}
static void wait_core_queue(KThread *t) {
    uint64_t end=plat_now_ns()+2000000000ull;
    for(;;) {
        pthread_mutex_lock(&c0_m);bool found=false;
        for(KThread *p=c0_head;p;p=p->core0_next)found|=p==t;
        pthread_mutex_unlock(&c0_m);
        if(found)return;
        assert(plat_now_ns()<end);usleep(100);
    }
}
static void wait_mutex_queue(KThread *t) {
    uint64_t end=plat_now_ns()+2000000000ull;
    for(;;) {
        pthread_mutex_lock(&g_klock);bool found=false;
        for(KThread *p=g_waitlist;p;p=p->wnext)found|=p==t && p->wait_kind==WAIT_MUTEX;
        pthread_mutex_unlock(&g_klock);
        if(found)return;
        assert(plat_now_ns()<end);usleep(100);
    }
}
static int effective(KThread *t) {
    pthread_mutex_lock(&g_klock);int value=thread_effective_priority(t);pthread_mutex_unlock(&g_klock);return value;
}
static void reset(void) {
    assert(!g_waitlist && !c0_head && !c0_holder && g_fnprof==1);
    kernel_reset();t_cur=NULL;
    assert(!c0_priorities && !c0_priority_capacity && !c0_mutex_waiters);
}
static KThread *created[120];
static void *create_batch(void *argument) {
    size_t first=(size_t)(uintptr_t)argument;
    for(size_t i=first;i<first+20;++i)created[i]=thread_new(0,0,0,44,0);
    return NULL;
}
static void test_parallel_registration(void) {
    pthread_t creators[6];bool ids[120]={0};
    for(size_t i=0;i<6;++i)assert(!pthread_create(&creators[i],NULL,create_batch,(void *)(uintptr_t)(i*20)));
    for(size_t i=0;i<6;++i)pthread_join(creators[i],NULL);
    for(size_t i=0;i<120;++i) {
        uint64_t index=created[i]->id-0x50;
        assert(index<120 && !ids[index]);ids[index]=true;
        assert(created[i]->tlr==TLS_BASE+index*0x200 && created[i]->cpu.tpidrro==created[i]->tlr);
    }
    reset();
}
static void test_owner_progress(void) {
    atomic_store(&medium_ran,0);
    KThread *low=new_thread(56),*medium=new_thread(44),*high=new_thread(20);
    t_cur=low;core0_acquire(low);mutex_word=low->handle|WAITERS_BIT;
    pthread_t a,b;assert(!pthread_create(&a,NULL,medium_main,medium));wait_core_queue(medium);
    assert(!pthread_create(&b,NULL,high_main,high));wait_mutex_queue(high);
    assert(effective(low)==(inheritance_enabled?20:56) && thread_priority(low)==56);
    kernel_preempt_point();
    assert(atomic_load(&medium_ran)==0 && "mutex owner must inherit its blocked high-priority waiter's priority");
    assert(svc_arbitrate_unlock((uintptr_t)&mutex_word)==R_OK);core0_release(low);
    pthread_join(a,NULL);pthread_join(b,NULL);
    assert(!mutex_word && !g_waitlist && !c0_head && !c0_holder && g_fnprof==1);
    assert(effective(low)==56);reset();
}

typedef struct {
    KThread *thread;
    uint32_t *word,*extra,*key;
    bool cancelled,record;
    int after_extra;
    int64_t timeout;
    uint32_t result;
} Wait;
static _Atomic unsigned order_count;
static uint64_t acquired[2];
static void *wait_main(void *argument) {
    Wait *w=argument;t_cur=w->thread;
    if(w->key)w->result=svc_wait_process_wide_key((uintptr_t)w->word,(uintptr_t)w->key,t_cur->handle,w->timeout);
    else w->result=svc_arbitrate_lock(__atomic_load_n(w->word,__ATOMIC_SEQ_CST)&~WAITERS_BIT,(uintptr_t)w->word,t_cur->handle);
    if(w->result==R_OK && !w->cancelled) {
        assert((__atomic_load_n(w->word,__ATOMIC_SEQ_CST)&~WAITERS_BIT)==t_cur->handle);
        if(w->record) { unsigned i=atomic_fetch_add(&order_count,1);assert(i<2);acquired[i]=t_cur->id; }
        if(w->extra) {
            assert(svc_arbitrate_unlock((uintptr_t)w->extra)==R_OK);
            assert(effective(t_cur)==(inheritance_enabled?w->after_extra:thread_priority(t_cur)));
        }
        assert(svc_arbitrate_unlock((uintptr_t)w->word)==R_OK);
    }
    t_cur=NULL;return NULL;
}
static pthread_t start_wait(Wait *w) {
    pthread_t thread;assert(!pthread_create(&thread,NULL,wait_main,w));return thread;
}
static void cancel_wait(KThread *t) {
    pthread_mutex_lock(&g_klock);t->cancel=true;wake(t);pthread_mutex_unlock(&g_klock);
}
static void test_chain_and_cancel(void) {
    KThread *a=new_thread(56),*b=new_thread(55),*high=new_thread(20),*second=new_thread(25);
    uint32_t ma=a->handle|WAITERS_BIT,mb=b->handle|WAITERS_BIT;
    Wait wa={.thread=a,.word=&mb},wh={.thread=high,.word=&ma,.cancelled=true},ws={.thread=second,.word=&ma,.cancelled=true};
    pthread_t ta=start_wait(&wa);wait_mutex_queue(a);
    pthread_t th=start_wait(&wh);wait_mutex_queue(high);
    assert(effective(a)==(inheritance_enabled?20:56) && effective(b)==(inheritance_enabled?20:55));
    pthread_mutex_lock(&g_klock);thread_set_priority(high,30);pthread_mutex_unlock(&g_klock);
    assert(effective(a)==(inheritance_enabled?30:56) && effective(b)==(inheritance_enabled?30:55));
    pthread_t ts=start_wait(&ws);wait_mutex_queue(second);
    assert(effective(a)==(inheritance_enabled?25:56) && effective(b)==(inheritance_enabled?25:55));
    cancel_wait(second);pthread_join(ts,NULL);
    assert(effective(a)==(inheritance_enabled?30:56) && effective(b)==(inheritance_enabled?30:55));
    cancel_wait(high);pthread_join(th,NULL);
    assert(effective(a)==56 && effective(b)==55);
    t_cur=b;assert(svc_arbitrate_unlock((uintptr_t)&mb)==R_OK);pthread_join(ta,NULL);
    t_cur=a;assert(svc_arbitrate_unlock((uintptr_t)&ma)==R_OK);
    assert(!ma && !mb);reset();
}
static void test_handoff(void) {
    KThread *owner=new_thread(56),*first=new_thread(45),*second=new_thread(30),*high=new_thread(20);
    uint32_t word=owner->handle|WAITERS_BIT,extra=first->handle|WAITERS_BIT;
    Wait w1={.thread=first,.word=&word,.extra=&extra,.record=true,.after_extra=30};
    Wait w2={.thread=second,.word=&word,.record=true},wh={.thread=high,.word=&extra};
    atomic_store(&order_count,0);
    pthread_t t1=start_wait(&w1);wait_mutex_queue(first);
    pthread_t t2=start_wait(&w2);wait_mutex_queue(second);
    pthread_t th=start_wait(&wh);wait_mutex_queue(high);
    assert(effective(owner)==(inheritance_enabled?20:56));
    t_cur=owner;assert(svc_arbitrate_unlock((uintptr_t)&word)==R_OK);
    pthread_join(t1,NULL);pthread_join(t2,NULL);pthread_join(th,NULL);
    assert(acquired[0]==(inheritance_enabled?first:second)->id);
    assert(acquired[1]==(inheritance_enabled?second:first)->id);
    assert(effective(owner)==56 && effective(first)==45 && !word && !extra);reset();
}
static void test_condition_reacquire(void) {
    KThread *low=new_thread(56),*high=new_thread(20);
    uint32_t word=high->handle,key=0;
    Wait w={.thread=high,.word=&word,.key=&key,.timeout=-1};pthread_t thread=start_wait(&w);
    uint64_t end=plat_now_ns()+2000000000ull;
    for(;;) {
        pthread_mutex_lock(&g_klock);bool waiting=high->wait_kind==WAIT_CONDVAR;pthread_mutex_unlock(&g_klock);
        if(waiting)break;assert(plat_now_ns()<end);usleep(100);
    }
    pthread_mutex_lock(&g_klock);assert(mutex_acquire_or_mark((uintptr_t)&word,low->handle));pthread_mutex_unlock(&g_klock);
    t_cur=low;svc_signal_process_wide_key((uintptr_t)&key,1);wait_mutex_queue(high);
    assert(effective(low)==(inheritance_enabled?20:56));
    assert(svc_arbitrate_unlock((uintptr_t)&word)==R_OK);pthread_join(thread,NULL);
    assert(w.result==R_OK && !word && !key && effective(low)==56);
    // A condition timeout creates no lingering mutex donation or ownership.
    word=high->handle;w.timeout=1000000;thread=start_wait(&w);pthread_join(thread,NULL);
    assert(w.result==R_TIMEOUT && effective(low)==56);
    if(g_game_version==TOTK_VERSION)assert(!word);
    else { assert(word==high->handle);t_cur=high;assert(svc_arbitrate_unlock((uintptr_t)&word)==R_OK); }
    reset();
}
static void test_invalid_owner(void) {
    KThread *high=new_thread(20);uint32_t word=0xdead|WAITERS_BIT;
    Wait w={.thread=high,.word=&word,.cancelled=true};pthread_t thread=start_wait(&w);wait_mutex_queue(high);
    assert(effective(high)==20);cancel_wait(high);pthread_join(thread,NULL);reset();
}
static void test_allocation_failure(void) {
    for(unsigned kind=0;kind<2;++kind) {
        pid_t child=fork();assert(child>=0);
        if(!child) {
            KThread *t=new_thread(56);
            if(kind)t->id=SIZE_MAX/sizeof(CorePriority);else fail_realloc=true;
            pthread_mutex_lock(&g_klock);core0_refresh_inheritance(true);pthread_mutex_unlock(&g_klock);
            _exit(1);
        }
        int status;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    }
}
int main(int argc,char **argv) {
    alarm(20);
    bool off=argc>1 && !strcmp(argv[1],"off");
    bool other=argc>1 && !strcmp(argv[1],"other-game");
    if(argc>1 && !strcmp(argv[1],"default"))unsetenv("LARECOMP_C0_PRIORITY");
    else setenv("LARECOMP_C0_PRIORITY",off?"0":"1",1);
    if(other)g_game_version=182;
    unsetenv("LARECOMP_C0_SLICE_MS");unsetenv("LARECOMP_LOG_KERNEL_WAITS");
    pthread_once(&c0_once,core0_init);
    inheritance_enabled=!other && (!TOTK_DIAGNOSTICS || !off);
    assert((c0_priority && g_game_version==TOTK_VERSION)==inheritance_enabled);
    kernel_reset();test_parallel_registration();
    test_owner_progress();test_chain_and_cancel();test_handoff();test_condition_reacquire();test_invalid_owner();
    if(inheritance_enabled && !(argc>1 && !strcmp(argv[1],"no-fault")))test_allocation_failure();
    printf("PASS: concurrent thread identity/TLS, mutex owner progress, transitive donation, cancellation, priority changes, handoff, condition reacquire/timeout, invalid owner and reset (enabled=%d)\n",inheritance_enabled);
}
