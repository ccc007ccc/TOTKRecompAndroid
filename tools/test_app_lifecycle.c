#define _POSIX_C_SOURCE 200809L
#include "../runtime/app_lifecycle.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static atomic_uint entered, finished;
static void delay(void) { struct timespec t={0,1000000};nanosleep(&t,NULL); }
static void until(atomic_uint *value,unsigned expected) {
    for(unsigned i=0;i<5000 && atomic_load(value)!=expected;++i)delay();
    assert(atomic_load(value)==expected);
}
static void *frame(void *unused) {
    (void)unused;
    atomic_fetch_add(&entered,1);
    app_lifecycle_wait();
    atomic_fetch_add(&finished,1);
    return NULL;
}
int main(void) {
    app_lifecycle_start();
    assert(!app_lifecycle_backgrounded());
    app_lifecycle_wait();
    for(unsigned round=0;round<10;++round) {
        atomic_store(&entered,0);atomic_store(&finished,0);
        app_lifecycle_set_foreground(false);
        pthread_t threads[3];
        for(unsigned i=0;i<3;++i)assert(!pthread_create(&threads[i],NULL,frame,NULL));
        until(&entered,3);
        for(unsigned i=0;i<10;++i)delay();
        assert(!atomic_load(&finished));
        if(round&1)app_lifecycle_stop();else app_lifecycle_set_foreground(true);
        until(&finished,3);
        for(unsigned i=0;i<3;++i)pthread_join(threads[i],NULL);
        if(round&1) {
            // Reboot releases waits but does not invent a foreground event.
            assert(app_lifecycle_backgrounded());
            app_lifecycle_start();
        }
    }
    app_lifecycle_set_foreground(true);
    app_lifecycle_wait();
    puts("PASS: foreground fast path, background sleep, all-waiter resume, shutdown and repeated boot/focus cycles");
}
