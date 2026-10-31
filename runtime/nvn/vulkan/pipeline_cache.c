// Persist both game and built-in driver pipelines. Eden #4294 likewise saves
// during play so Android process termination does not lose the session's work.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "vk.h"
#include "device_compat.h"
#include "pipeline_warm.h"

static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;

bool vk_pipeline_cache_save(void) {
    if (!g_vk.dev || !g_vk.pcache || getenv("LARECOMP_VK_NO_PIPELINE_CACHE")) return false;
    vk_pipeline_warm_save();
    pthread_mutex_lock(&save_lock);
    bool saved = false;
    // Other workers can grow the driver's cache between the size query and read.
    // Retry boundedly; a failed snapshot must not replace the previous file.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        size_t size = 0;
        VkResult result = vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &size, NULL);
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS || size < 32) break;
        if (size > VK_BACKEND_PIPELINE_CACHE_MAX) {
            static bool reported_limit;
            if (!reported_limit) {
                LOG("[vk-cache] snapshot %zu bytes exceeds %u-byte limit; retaining the previous cache\n",
                    size, VK_BACKEND_PIPELINE_CACHE_MAX);
                reported_limit = true;
            }
            break;
        }
        size_t capacity = size;
        void *data = malloc(capacity);
        if (!data) break;
        result = vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &size, data);
        if (result == VK_INCOMPLETE) { free(data); continue; }
        if (result != VK_SUCCESS || size > capacity || !vk_backend_cache_compatible(data, size, &g_vk.props)) {
            free(data); break;
        }
        mkdir("cache", 0755);
        const char *temporary = "cache/vk_pipelines.bin.tmp", *destination = "cache/vk_pipelines.bin";
        FILE *file = fopen(temporary, "wb");
        if (file) {
            bool complete = fwrite(data, 1, size, file) == size;
            if (fclose(file)) complete = false;
            if (complete) {
#ifdef _WIN32
                saved = MoveFileExA(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
                saved = rename(temporary, destination) == 0;
#endif
            }
            if (!saved) remove(temporary);
        }
        free(data);
        break;
    }
    pthread_mutex_unlock(&save_lock);
    return saved;
}
