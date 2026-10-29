// Exercise the production loader with a real compiler: strict misses, fallback,
// cache repair, concurrent first use, and gameplay without writable cache storage.
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/shaders.c"

VkG g_vk;
void rt_fatal_trace(void) { abort(); }

static const char fragment[] = "#version 450\nlayout(location=0) out vec4 c;\n"
                               "void main(){c=vec4(0.25,0.5,0.75,1.0);}\n";
static const char compute[] = "#version 450\nlayout(local_size_x=1) in;\nvoid main(){}\n";

static void expect_abort(const char *source, bool strict) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        size_t n;
        if (strict) vk_load_prepared_shader(source, STAGE_FRAGMENT, "strict", &n);
        else vk_load_shader(source, STAGE_FRAGMENT, "expected-failure", &n);
        _exit(99);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

static uint64_t load_valid(const char *source, int stage) {
    size_t n;
    uint32_t *spv = vk_load_shader(source, stage, "runtime-test", &n);
    assert(spv && vk_shader_cache_framed(spv, n));
    assert(!prepare_library && !prepare_context && !prepare_compile);
    uint64_t digest = payload_hash(spv, n);
    free(spv);
    return digest;
}

static void *first_use(void *arg) {
    *(uint64_t *)arg = load_valid(compute, STAGE_COMPUTE);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char directory[1200];
    snprintf(directory, sizeof directory, "%s/runtime-XXXXXX", argv[1]);
    assert(mkdtemp(directory) && !chdir(directory));
    char *library = strdup(getenv("LARECOMP_NATIVE_LIB_DIR")); assert(library);
    setenv("LARECOMP_SHADER_RUNTIME_COMPILE", "0", 1);
    assert(!vk_shader_runtime_compile_enabled());
    expect_abort(fragment, false);
    assert(access("cache", F_OK) != 0);

    setenv("LARECOMP_SHADER_RUNTIME_COMPILE", "1", 1);
    assert(vk_shader_runtime_compile_enabled());
    expect_abort(fragment, true); // The audit/preparation API stays strict, even with fallback enabled.
    uint64_t original = load_valid(fragment, STAGE_FRAGMENT);
    size_t n;
    uint32_t *cached = cache_load(source_key(fragment, STAGE_FRAGMENT), &n);
    assert(cached && payload_hash(cached, n) == original); free(cached);

    setenv("LARECOMP_NATIVE_LIB_DIR", "/unavailable-shader-compiler", 1);
    assert(load_valid(fragment, STAGE_FRAGMENT) == original); // Enabled hits never load the compiler.
    setenv("LARECOMP_SHADER_RUNTIME_COMPILE", "0", 1);
    assert(load_valid(fragment, STAGE_FRAGMENT) == original);
    char path[192]; cache_path(path, sizeof path, source_key(fragment, STAGE_FRAGMENT));
    assert(!truncate(path, sizeof(ShaderEnvelope) + 1));
    expect_abort(fragment, false);
    setenv("LARECOMP_NATIVE_LIB_DIR", library, 1);
    setenv("LARECOMP_SHADER_RUNTIME_COMPILE", "1", 1);
    assert(load_valid(fragment, STAGE_FRAGMENT) == original);
    expect_abort("#version 450\ninvalid", false);

    pthread_t threads[6]; uint64_t digests[6];
    for (unsigned i = 0; i < 6; ++i) assert(!pthread_create(&threads[i], NULL, first_use, &digests[i]));
    for (unsigned i = 0; i < 6; ++i) { assert(!pthread_join(threads[i], NULL)); assert(digests[i] == digests[0]); }
    cached = cache_load(source_key(compute, STAGE_COMPUTE), &n); assert(cached); free(cached);
    assert(access(".shaders-ready", F_OK) != 0);

    assert(!mkdir("no-cache-storage", 0755) && !chdir("no-cache-storage"));
    FILE *file = fopen("cache", "wb"); assert(file && !fclose(file)); // Directory creation cannot succeed.
    assert(load_valid(fragment, STAGE_FRAGMENT) == original);
    assert(access(".shaders-ready", F_OK) != 0);
    free(library);
    puts("PASS: strict mode, enabled/disabled cache hits, fallback/repair, compiler errors, concurrent first use, unwritable cache");
}
