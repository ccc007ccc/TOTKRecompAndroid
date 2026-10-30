#include "compile_budget.h"
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static unsigned loading_limit, playing_limit, active, background_active, foreground_waiters, peak;
static bool loading;
static _Thread_local bool background, admitted_background;
static _Thread_local VkCompileBudgetCheck cancelled, urgent;
static _Thread_local void *context;
static _Thread_local VkCompileBudgetDependency dependency;
static void initialize(void) {
#ifdef _WIN32
    unsigned cores=4;
#else
    long online=sysconf(_SC_NPROCESSORS_ONLN);unsigned cores=online>0?(unsigned)online:1;
#endif
    loading_limit=cores>1?cores-1:1;if(loading_limit>4)loading_limit=4;
    playing_limit=cores/3;if(!playing_limit)playing_limit=1;if(playing_limit>3)playing_limit=3;
}
unsigned vk_compile_budget_workers(void) {pthread_once(&once,initialize);return loading_limit;}
bool vk_compile_budget_background(bool value) {bool old=background;background=value;return old;}
bool vk_compile_budget_is_background(void) {return background;}
bool vk_compile_budget_cancelled(void) {return cancelled && cancelled(context);}
VkCompileBudgetDependency vk_compile_budget_dependency(VkCompileBudgetDependency value) {VkCompileBudgetDependency old=dependency;dependency=value;return old;}
void vk_compile_budget_checks(VkCompileBudgetCheck c,VkCompileBudgetCheck u,void *p) {cancelled=c;urgent=u;context=p;}
void vk_compile_budget_loading(bool value) {
    pthread_once(&once,initialize);pthread_mutex_lock(&lock);loading=value;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);
}
void vk_compile_budget_wake(void) {pthread_mutex_lock(&lock);pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);}
bool vk_compile_budget_acquire(void) {
    pthread_once(&once,initialize);pthread_mutex_lock(&lock);
    bool registered=false;
    for(;;) {
        bool front=!background || (urgent && urgent(context)) || (dependency.check && dependency.check(dependency.context));
        if(front && !registered){++foreground_waiters;registered=true;}
        if(cancelled && cancelled(context)) {
            if(registered)--foreground_waiters;
            pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);return false;
        }
        unsigned limit=loading?loading_limit:playing_limit;
        unsigned bg_limit=loading && limit>1?limit-1:1;
        if(active<limit && (front || (!foreground_waiters && background_active<bg_limit))) {
            if(registered)--foreground_waiters;
            admitted_background=!front;++active;if(admitted_background)++background_active;
            if(active>peak)peak=active;
            pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);return true;
        }
        // Poll task-local cancellation/promotion without requiring callers to
        // enter our mutex from lifecycle watches or other cache locks.
        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
        until.tv_nsec+=50000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
        pthread_cond_timedwait(&changed,&lock,&until);
    }
}
void vk_compile_budget_release(void) {
    pthread_mutex_lock(&lock);--active;if(admitted_background)--background_active;
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);
}
VkCompileBudgetStats vk_compile_budget_stats(void) {
    pthread_once(&once,initialize);pthread_mutex_lock(&lock);
    VkCompileBudgetStats s={active,background_active,foreground_waiters,peak,loading?loading_limit:playing_limit};
    pthread_mutex_unlock(&lock);return s;
}
