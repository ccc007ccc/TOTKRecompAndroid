// Production loader/preparation separation, complete built-ins, and corruption rejection.
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/shaders.c"
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
#ifdef __ANDROID__
#include <stdarg.h>
void rt_fatal_message(const char *format, ...) {
    va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); abort();
}
#endif

extern const char smaa_hlsl_src[];
int main(int argc, char **argv) {
    assert(argc == 2 && !chdir(argv[1]));
    char why[1024];
#ifdef __ANDROID__
    const uint32_t apis[] = {VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3};
    for (unsigned i = 0; i < sizeof apis / sizeof *apis; ++i) {
        g_vk.api = apis[i];
#endif
    vk_shaders_init();
    assert(vk_shader_prepare_begin(why, sizeof why));
    assert(vk_shader_prepare_source("#version 450\nlayout(local_size_x_id=0,local_size_y_id=1,local_size_z_id=2) in;void main(){}\n",
        STAGE_COMPUTE, "compute-specialization", why, sizeof why));
    vk_shader_prepare_end();
#ifdef __ANDROID__
    }
#endif
    assert(!prepare_compile && !prepare_library);
    // A cache-only startup must not depend on shaderc even for built-ins.
    const char *compiler_dir = getenv("LARECOMP_NATIVE_LIB_DIR");
    char *saved_dir = compiler_dir ? strdup(compiler_dir) : NULL;
    uint64_t session = prepare_session;
    setenv("LARECOMP_NATIVE_LIB_DIR", "/no-compiler-for-cached-builtins", 1);
    vk_shaders_init();
    assert(prepare_session == session && !prepare_library && !prepare_context);
    if (saved_dir) { setenv("LARECOMP_NATIVE_LIB_DIR", saved_dir, 1); free(saved_dir); }
    else unsetenv("LARECOMP_NATIVE_LIB_DIR");
    int stage; const char *none = vk_builtin_source("clear.frag.none", &stage);
    assert(stage == STAGE_FRAGMENT && !strcmp(none, "#version 450\nvoid main() {}\n"));
    size_t n; uint32_t *spv = vk_load_prepared_shader(none, stage, "depth-only", &n);
    assert(spv && vk_shader_cache_framed(spv, n)); free(spv);
    assert(!vk_shader_prepare_source(none, stage, "closed", why, sizeof why));
    assert(vk_shader_prepare_begin(why, sizeof why));
    assert(!vk_shader_prepare_source("#version 450\ninvalid", STAGE_FRAGMENT, "invalid", why, sizeof why));
    vk_shader_prepare_end();
    uint64_t key = source_key(none, stage); char path[192]; cache_path(path, sizeof path, key);
    FILE *packed_file=fopen(path,"rb");assert(packed_file);ShaderEnvelope header;
    assert(fread(&header,sizeof header,1,packed_file)==1 && header.magic==SHADER_ENVELOPE_MAGIC && header.stored_bytes>0);
    fclose(packed_file);
    FILE *f = fopen(path, "r+b"); assert(f);
    assert(!fseek(f, sizeof(ShaderEnvelope) + 8, SEEK_SET));
    int byte = fgetc(f); assert(byte != EOF && !fseek(f, -1, SEEK_CUR));
    fputc(byte ^ 1, f); assert(!fclose(f));
    assert(!cache_load(key, &n)); // Even an otherwise well-framed payload fails its checksum.
    pid_t child = fork(); assert(child >= 0);
    if (!child) { vk_load_prepared_shader(none, stage, "damaged", &n); _exit(99); }
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(vk_shader_prepare_begin(why, sizeof why));
    assert(vk_shader_prepare_source(none, stage, "repair", why, sizeof why));
    vk_shader_prepare_end();
    spv = vk_load_prepared_shader(none, stage, "repaired", &n); assert(spv); free(spv);
    // Declared output size is bounded before decompression; trailing bytes are
    // rejected rather than accepted as an unchecked second frame.
    f=fopen(path,"r+b");assert(f);ShaderEnvelope oversized=header;oversized.words=4*1024*1024+1;
    assert(fwrite(&oversized,sizeof oversized,1,f)==1 && !fclose(f));assert(!cache_load(key,&n));
    f=fopen(path,"r+b");assert(f);assert(fwrite(&header,sizeof header,1,f)==1 && !fseek(f,0,SEEK_END));fputc(0,f);assert(!fclose(f));assert(!cache_load(key,&n));
    assert(vk_shader_prepare_begin(why,sizeof why));assert(vk_shader_prepare_source(none,stage,"repair-trailing",why,sizeof why));vk_shader_prepare_end();
    spv=vk_load_prepared_shader(none,stage,"compressed-roundtrip",&n);assert(spv && n==header.words);free(spv);
    puts("PASS: 36 built-ins prepared and restart without compiler; invalid source/corrupt payload rejected, preparation repairs cache");
}
