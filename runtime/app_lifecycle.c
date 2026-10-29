#include "app_lifecycle.h"
#include <pthread.h>
#include <stdatomic.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static atomic_bool background, stopping;

void app_lifecycle_set_foreground(bool foreground) {
    pthread_mutex_lock(&lock);
    atomic_store_explicit(&background, !foreground, memory_order_release);
    if (foreground) pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

bool app_lifecycle_backgrounded(void) {
    return atomic_load_explicit(&background, memory_order_acquire);
}

void app_lifecycle_wait(void) {
    if (!app_lifecycle_backgrounded()) return;
    pthread_mutex_lock(&lock);
    while (atomic_load_explicit(&background, memory_order_acquire) &&
           !atomic_load_explicit(&stopping, memory_order_acquire))
        pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}

void app_lifecycle_stop(void) {
    pthread_mutex_lock(&lock);
    atomic_store_explicit(&stopping, true, memory_order_release);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

void app_lifecycle_start(void) {
    atomic_store_explicit(&stopping, false, memory_order_release);
}
