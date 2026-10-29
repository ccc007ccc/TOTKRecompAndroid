// Cross-namespace decoding uses real complete/dictionary modules, with the
// compiler disabled while reading the reviewed legacy fixtures.
#define SHADER_BUILD_ID "renderer-build"
#define SHADER_CACHE_ID "compiler-contract"
#define SHADER_COMPAT_CACHE_IDS "legacy-one", "legacy-two",
#include <assert.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/shaders.c"

VkG g_vk;
void rt_fatal_trace(void){abort();}
static const char vs[]="#version 450\nlayout(location=0) in vec4 a;void main(){gl_Position=a;}\n";
static const char vu[]="#version 450\nlayout(location=0) in uvec4 a;void main(){gl_Position=uintBitsToFloat(a);}\n";
static const char vi[]="#version 450\nlayout(location=0) in ivec4 a;void main(){gl_Position=uintBitsToFloat(uvec4(a));}\n";
static const char cs[]="#version 450\nlayout(local_size_x=1) in;void main(){}\n";
static void copy_file(const char* from,const char* to) {
    FILE *input=fopen(from,"rb"),*output=fopen(to,"wb");assert(input && output);
    char data[4096];size_t n;while((n=fread(data,1,sizeof data,input)))assert(fwrite(data,1,n,output)==n);
    assert(!ferror(input));assert(!fclose(input) && !fclose(output));
}
static void path(char* out,const char* ns,uint64_t key,bool base) {
    snprintf(out,256,"cache/shaders/%s/%016llx%s.bin",ns,(unsigned long long)key,base?".base":"");
}
static uint64_t load(uint64_t key) {
    size_t words=0;uint32_t* spv=vk_shader_cache_load_key(key,false,&words);assert(spv && words>5);
    uint64_t digest=payload_hash(spv,words);free(spv);return digest;
}
int main(int argc,char** argv) {
    assert(argc==2);char directory[1200];snprintf(directory,sizeof directory,"%s/compat-XXXXXX",argv[1]);assert(mkdtemp(directory) && !chdir(directory));
    char* library=strdup(getenv("LARECOMP_NATIVE_LIB_DIR"));assert(library);char why[2048];
    assert(vk_shader_prepare_begin(why,sizeof why));
    ShaderPrepareBase* base=vk_shader_prepare_base(vs,0,why,sizeof why);assert(base);
    ShaderPrepareVariant f={.key=source_key(vs,0)},u={.key=source_key(vu,0)};u.kinds[0]=2;
    assert(vk_shader_prepare_variant(base,&f,why,sizeof why) && vk_shader_prepare_variant(base,&u,why,sizeof why));
    vk_shader_prepare_base_free(base);assert(vk_shader_prepare_source(cs,5,"compute",why,sizeof why));vk_shader_prepare_end();
    uint64_t expected=load(u.key),compute=source_key(cs,5);load(compute);
    assert(!strcmp(vk_shader_cache_namespace(),"compiler-contract"));
    assert(!rename("cache/shaders/compiler-contract","cache/shaders/legacy-one"));
    setenv("LARECOMP_NATIVE_LIB_DIR","/compiler-unavailable",1);
    assert(load(u.key)==expected);load(f.key);load(compute);
    assert(!prepare_library && !prepare_context && !prepare_compile);
    ShaderCacheStats stats=vk_shader_cache_stats();assert(stats.compatible_hits>=3);
    assert(access("cache/shaders/compiler-contract",F_OK)); // a hit never copies or rewrites the old cache

    // An old base can generate a new variant without recompilation, but the
    // newly written module must be independently loadable in the current root.
    setenv("LARECOMP_NATIVE_LIB_DIR",library,1);
    assert(vk_shader_prepare_begin(why,sizeof why));
    base=vk_shader_prepare_base(vs,0,why,sizeof why);assert(base);
    assert(vk_shader_prepare_variant(base,&u,why,sizeof why));
    assert(access("cache/shaders/compiler-contract",F_OK));
    ShaderPrepareVariant signed_variant={.key=source_key(vi,0)};signed_variant.kinds[0]=1;
    char migrated_base[256],migrated_variant[256];
    path(migrated_base,"compiler-contract",f.key,true);
    path(migrated_variant,"compiler-contract",signed_variant.key,false);
    assert(!mkdir("cache/shaders/compiler-contract",0755) && !mkdir(migrated_base,0755));
    assert(!vk_shader_prepare_variant(base,&signed_variant,why,sizeof why));
    assert(access(migrated_variant,F_OK)); // no dependent without its dictionary
    assert(!rmdir(migrated_base));
    assert(vk_shader_prepare_variant(base,&signed_variant,why,sizeof why));
    vk_shader_prepare_base_free(base);vk_shader_prepare_end();
    setenv("LARECOMP_NATIVE_LIB_DIR","/compiler-unavailable",1);
    load(signed_variant.key);
    assert(!access(migrated_base,F_OK));
    assert(!remove(migrated_variant) && !remove(migrated_base) && !rmdir("cache/shaders/compiler-contract"));

    char build_module[256],legacy_compute[256];
    path(build_module,"renderer-build",compute,false);path(legacy_compute,"legacy-one",compute,false);
    assert(!mkdir("cache/shaders/renderer-build",0755) && !rename(legacy_compute,build_module));
    load(compute); // caches produced earlier by this same renderer build also work

    char legacy_base[256],other_base[256],legacy_variant[256],other_variant[256],current_base[256];
    path(legacy_base,"legacy-one",f.key,true);path(other_base,"legacy-two",f.key,true);
    path(legacy_variant,"legacy-one",u.key,false);path(other_variant,"legacy-two",u.key,false);
    path(current_base,"compiler-contract",f.key,true);
    assert(!mkdir("cache/shaders/legacy-two",0755));assert(!rename(legacy_base,other_base));
    assert(!mkdir("cache/shaders/compiler-contract",0755));copy_file(other_base,current_base);
    size_t words;assert(!vk_shader_cache_load_key(u.key,false,&words)); // cannot borrow another root's dictionary
    copy_file(legacy_variant,other_variant);
    assert(load(u.key)==expected); // complete pair in the next reviewed root is accepted
    assert(!truncate(legacy_variant,sizeof(ShaderEnvelope)+1));assert(load(u.key)==expected);
    assert(!rename("cache/shaders/legacy-two","cache/shaders/unreviewed"));
    assert(!vk_shader_cache_load_key(u.key,false,&words)); // unreviewed folders are never scanned
    char good_variant[256],current_variant[256];path(good_variant,"unreviewed",u.key,false);path(current_variant,"compiler-contract",u.key,false);
    copy_file(good_variant,current_variant);assert(load(u.key)==expected); // current namespace wins
    setenv("LARECOMP_NATIVE_LIB_DIR",library,1);assert(vk_shader_prepare_begin(why,sizeof why));
    const char fresh[]="#version 450\nlayout(local_size_x=2) in;void main(){}\n";
    assert(vk_shader_prepare_source(fresh,5,"new",why,sizeof why));vk_shader_prepare_end();
    char new_file[256],old_file[256];path(new_file,"compiler-contract",source_key(fresh,5),false);path(old_file,"legacy-one",source_key(fresh,5),false);
    assert(!access(new_file,F_OK) && access(old_file,F_OK));
    stats=vk_shader_cache_stats();assert(stats.current_hits>0 && stats.compatible_hits>0 && stats.misses>0);
    free(library);
    puts("PASS: stable namespace, read-only legacy hits, same-root dictionaries, new variants from legacy bases, corruption fallback, unreviewed rejection, current-only writes");
}
