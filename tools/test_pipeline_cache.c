// Real filesystem persistence with simulated driver growth/failure. The
// production writer must retain an old cache until a complete replacement exists.
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/pipeline_cache.c"

VkG g_vk;
bool vk_pipeline_warm_save(void) { return true; }
enum { NORMAL, GROW_ONCE, INCOMPLETE, OVERSIZE, BAD_HEADER };
static unsigned mode, queries, reads;
static uint8_t tone = 0x31;

static void put(uint8_t *data, unsigned offset, uint32_t word) { memcpy(data + offset, &word, 4); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t *size, void *data) {
    assert(device == g_vk.dev && cache == g_vk.pcache);
    size_t needed = mode == GROW_ONCE && reads ? 128 : 64;
    if (!data) { ++queries; *size = mode == OVERSIZE ? VK_BACKEND_PIPELINE_CACHE_MAX + 1ull : needed; return VK_SUCCESS; }
    ++reads;
    if (mode == INCOMPLETE || (mode == GROW_ONCE && reads == 1)) return VK_INCOMPLETE;
    assert(*size >= needed); *size = needed;
    uint8_t *bytes = data; memset(bytes, tone, needed);
    put(bytes, 0, 32); put(bytes, 4, VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
    put(bytes, 8, g_vk.props.vendorID); put(bytes, 12, g_vk.props.deviceID);
    memcpy(bytes + 16, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE);
    if (mode == BAD_HEADER) bytes[16] ^= 1;
    return VK_SUCCESS;
}
static void check_file(size_t size, uint8_t payload) {
    FILE *file = fopen("cache/vk_pipelines.bin", "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END) && ftell(file) == (long)size);
    assert(!fseek(file, 32, SEEK_SET) && fgetc(file) == payload && !fclose(file));
}
static void *save(void *unused) { (void)unused; assert(vk_pipeline_cache_save()); return NULL; }
int main(int argc, char **argv) {
    assert(argc == 2);
    char directory[1200]; snprintf(directory, sizeof directory, "%s/pipeline-XXXXXX", argv[1]);
    assert(mkdtemp(directory) && !chdir(directory));
    g_vk.dev = (VkDevice)(uintptr_t)1; g_vk.pcache = (VkPipelineCache)(uintptr_t)2;
    g_vk.props.vendorID = 0x5143; g_vk.props.deviceID = 0x44050001;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) g_vk.props.pipelineCacheUUID[i] = i + 1;
    mode = GROW_ONCE;
    assert(vk_pipeline_cache_save() && queries == 2 && reads == 2); check_file(128, 0x31);

    mode = INCOMPLETE; queries = reads = 0;
    assert(!vk_pipeline_cache_save() && queries == 3 && reads == 3); check_file(128, 0x31);
    mode = OVERSIZE; queries = reads = 0;
    assert(!vk_pipeline_cache_save() && queries == 1 && reads == 0); check_file(128, 0x31);
    mode = NORMAL; tone = 0x32;
    assert(!mkdir("cache/vk_pipelines.bin.tmp", 0755));
    assert(!vk_pipeline_cache_save()); check_file(128, 0x31);
    assert(!rmdir("cache/vk_pipelines.bin.tmp"));
    assert(!symlink("/dev/full", "cache/vk_pipelines.bin.tmp"));
    assert(!vk_pipeline_cache_save()); check_file(128, 0x31);
    assert(access("cache/vk_pipelines.bin.tmp", F_OK) != 0);

    tone = 0x34; queries = reads = 0;
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, save, NULL));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(queries == 4 && reads == 4); check_file(64, 0x34);
    mode = BAD_HEADER;
    assert(!vk_pipeline_cache_save()); check_file(64, 0x34);
    setenv("LARECOMP_VK_NO_PIPELINE_CACHE", "1", 1);
    unsigned previous = queries;
    assert(!vk_pipeline_cache_save() && queries == previous); check_file(64, 0x34);
    puts("PASS: growing cache retry, bounded failures, concurrent saves, write/close errors, atomic replacement, driver identity");
}
