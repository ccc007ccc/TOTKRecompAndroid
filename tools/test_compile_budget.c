#include "../runtime/nvn/vulkan/compile_budget.h"
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
static _Atomic unsigned held,finished,order;
static _Atomic bool cancelled,urgent,release_front;
static _Atomic unsigned release_holders,front_order,bg_order;
static bool is_cancelled(void *p){(void)p;return cancelled;}
static bool is_urgent(void *p){(void)p;return urgent;}
static void *holder(void *p){assert(vk_compile_budget_acquire());++held;while(release_holders<=(unsigned)(size_t)p)usleep(100);vk_compile_budget_release();return NULL;}
static void *front(void *p){(void)p;assert(vk_compile_budget_acquire());front_order=++order;while(!release_front)usleep(100);vk_compile_budget_release();return NULL;}
static void *back(void *p){(void)p;vk_compile_budget_background(true);vk_compile_budget_checks(is_cancelled,is_urgent,NULL);bool ok=vk_compile_budget_acquire();if(ok){bg_order=++order;vk_compile_budget_release();}++finished;return NULL;}
int main(void){
    vk_compile_budget_loading(true);unsigned limit=vk_compile_budget_stats().limit;assert(limit>=1 && limit<=4);
    pthread_t holders[4],f,b;
    for(unsigned i=0;i<limit;++i)assert(!pthread_create(&holders[i],NULL,holder,(void*)(size_t)i));
    while(held<limit)usleep(100);
    assert(!pthread_create(&b,NULL,back,NULL));assert(!pthread_create(&f,NULL,front,NULL));
    while(!vk_compile_budget_stats().waiting)usleep(100);
    // Cancellation wakes a queued task even while every compiler slot is busy.
    cancelled=true;vk_compile_budget_wake();pthread_join(b,NULL);assert(finished==1 && !bg_order);
    cancelled=false;assert(!pthread_create(&b,NULL,back,NULL));
    // Release one slot: foreground admission precedes queued background work.
    release_holders=1;while(!front_order)usleep(100);
    release_holders=limit;for(unsigned i=0;i<limit;++i)pthread_join(holders[i],NULL);
    release_front=true;pthread_join(f,NULL);pthread_join(b,NULL);
    assert(front_order==1 && bg_order==2 && !vk_compile_budget_stats().active && vk_compile_budget_stats().peak==limit);
    // Promoted needed work participates as foreground, with no second ticket.
    urgent=true;assert(!pthread_create(&b,NULL,back,NULL));pthread_join(b,NULL);assert(finished==3);
    vk_compile_budget_loading(false);assert(vk_compile_budget_stats().limit<=3);
    puts("PASS: shared loading/play limits, foreground priority, queued cancellation, promotion and balanced admission");
}
