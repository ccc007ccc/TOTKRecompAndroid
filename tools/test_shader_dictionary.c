// Production dictionary storage, dependency validation and preparation-only repair.
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/shaders.c"
VkG g_vk;
void rt_fatal_trace(void) { abort(); }

static ShaderPrepareCompileContext compiler;
static ShaderPrepareDerive derive;
static _Atomic unsigned compiled, derived;
static uint32_t *count_compile(void *ctx,const char *src,int stage,unsigned api,bool opt,size_t *n,char *why,size_t cap) {
    ++compiled;return compiler(ctx,src,stage,api,opt,n,why,cap);
}
static uint32_t *count_derive(const uint32_t *spv,size_t words,int stage,const uint8_t *kinds,unsigned api,size_t *n,char *why,size_t cap) {
    ++derived;return derive(spv,words,stage,kinds,api,n,why,cap);
}
static char why[8192];
static void begin(void) {
    assert(vk_shader_prepare_begin(why,sizeof why));compiler=prepare_compile;derive=prepare_derive;
    prepare_compile=count_compile;prepare_derive=count_derive;
}
static const char *sources[]={
    "#version 450\nlayout(location=0) in vec4 a;void main(){gl_Position=uintBitsToFloat(floatBitsToUint(a));}",
    "#version 450\nlayout(location=0) in ivec4 a;void main(){gl_Position=uintBitsToFloat(uvec4(a));}",
    "#version 450\nlayout(location=0) in uvec4 a;void main(){gl_Position=uintBitsToFloat(a);}",
};
static ShaderPrepareVariant variants[3];
static void prepare(void) {
    begin();ShaderPrepareBase *base=vk_shader_prepare_base(sources[0],STAGE_VERTEX,why,sizeof why);assert(base);
    for(unsigned i=0;i<3;++i)assert(vk_shader_prepare_variant(base,&variants[i],why,sizeof why));
    vk_shader_prepare_base_free(base);vk_shader_prepare_end();
    assert(!prepare_compile && !prepare_derive && !prepare_context && !prepare_library);
}
static ShaderEnvelope header(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);ShaderEnvelope h;assert(fread(&h,sizeof h,1,f)==1 && !fclose(f));return h;
}
static void put_header(const char *path,ShaderEnvelope h) {
    FILE *f=fopen(path,"r+b");assert(f);assert(fwrite(&h,sizeof h,1,f)==1 && !fclose(f));
}
static void corrupt(const char *path) {
    FILE *f=fopen(path,"r+b");assert(f && !fseek(f,sizeof(ShaderEnvelope)+8,SEEK_SET));
    int c=fgetc(f);assert(c!=EOF && !fseek(f,-1,SEEK_CUR));assert(fputc(c^0x20,f)!=EOF && !fclose(f));
}
static void readable(void) {
    for(unsigned i=0;i<3;++i){size_t n;uint32_t *spv=vk_load_prepared_shader(sources[i],STAGE_VERTEX,"dictionary",&n);assert(spv);free(spv);}
}
static void *cross_thread(void *base) {
    char error[1024];assert(vk_shader_prepare_begin(error,sizeof error));
    assert(!vk_shader_prepare_variant(base,&variants[0],error,sizeof error));vk_shader_prepare_end();return NULL;
}
static void *shared_base(void *unused) {
    (void)unused;char error[1024];assert(vk_shader_prepare_begin(error,sizeof error));
    prepare_compile=count_compile;prepare_derive=count_derive;
    ShaderPrepareBase *base=vk_shader_prepare_base(sources[0],STAGE_VERTEX,error,sizeof error);assert(base);
    assert(vk_shader_prepare_variant(base,&variants[0],error,sizeof error));
    vk_shader_prepare_base_free(base);vk_shader_prepare_end();return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2 && !chdir(argv[1]));unsetenv("LARECOMP_VK_OPT");
    char base_file[192],variant_file[192];
    for(unsigned i=0;i<3;++i){variants[i].key=source_key(sources[i],STAGE_VERTEX);variants[i].kinds[0]=i;
        char path[192];cache_path(path,sizeof path,variants[i].key);remove(path);}
    base_path(base_file,sizeof base_file,variants[0].key);remove(base_file);
    cache_path(variant_file,sizeof variant_file,variants[1].key);
    prepare();assert(compiled==1 && derived==2);readable();
    ShaderEnvelope b=header(base_file),v=header(variant_file);
    assert(b.encoding==SHADER_STANDALONE && v.encoding==SHADER_DICTIONARY && v.base_key==b.key && v.base_digest==b.digest);
    // Repeated groups load one base and reuse all complete results without shaderc/derive.
    prepare();assert(compiled==1 && derived==2);
    begin();ShaderPrepareBase *base=vk_shader_prepare_base(sources[0],STAGE_VERTEX,why,sizeof why);assert(base);
    for(unsigned i=1;i<3;++i) {
        size_t n,expected_n;uint32_t *expected=derive(base->spv,base->words,STAGE_VERTEX,variants[i].kinds,shader_api(),&expected_n,why,sizeof why);
        uint32_t *loaded=cache_load(variants[i].key,&n);assert(expected && loaded && n==expected_n && !memcmp(loaded,expected,n*4));free(loaded);free(expected);
    }
    ShaderPrepareVariant bad=variants[1];bad.kinds[4]=3;
    assert(!vk_shader_prepare_variant(base,&bad,why,sizeof why));
    pthread_t other;assert(!pthread_create(&other,NULL,cross_thread,base) && !pthread_join(other,NULL));
    vk_shader_prepare_end();
    assert(!vk_shader_prepare_variant(base,&variants[0],why,sizeof why));
    assert(!vk_shader_prepare_base(sources[0],STAGE_VERTEX,why,sizeof why));
    begin();assert(!vk_shader_prepare_variant(base,&variants[0],why,sizeof why));vk_shader_prepare_base_free(base);vk_shader_prepare_end();
    size_t n=99;
    // Missing/corrupt dictionaries fail all dependents. Preparation repairs the
    // base first and reuses deterministic, already-valid derived payloads.
    assert(!remove(base_file));assert(!cache_load(variants[1].key,&n) && !n);
    prepare();assert(compiled==2 && derived==2);readable();
    corrupt(base_file);assert(!cache_load(variants[1].key,&n));prepare();assert(compiled==3 && derived==2);
    corrupt(variant_file);assert(!cache_load(variants[1].key,&n));prepare();assert(compiled==3 && derived==3);
    // A dictionary's own valid envelope is insufficient: its digest must match
    // the dependent, and base entries can never reference another dictionary.
    ShaderEnvelope changed=v;changed.base_digest^=1;put_header(variant_file,changed);assert(!cache_load(variants[1].key,&n));
    changed=v;changed.base_key^=1;put_header(variant_file,changed);assert(!cache_load(variants[1].key,&n));put_header(variant_file,v);
    changed=b;changed.encoding=SHADER_DICTIONARY;changed.base_key=b.key;changed.base_digest=b.digest;
    put_header(base_file,changed);assert(!cache_load(variants[1].key,&n));put_header(base_file,b);
    changed=v;changed.encoding=2;put_header(variant_file,changed);assert(!cache_load(variants[1].key,&n));put_header(variant_file,v);
    // v2 caches and malformed dependent frames must be replaced by preparation.
    changed=v;changed.magic=0x325650534b544f54ull;put_header(variant_file,changed);assert(!cache_load(variants[1].key,&n));
    prepare();assert(compiled==3 && derived==4);
    FILE *f=fopen(variant_file,"ab");assert(f && fputc(0,f)!=EOF && !fclose(f));assert(!cache_load(variants[1].key,&n));
    prepare();assert(compiled==3 && derived==5);readable();
    begin();const char *compute="#version 450\nlayout(local_size_x_id=0) in;void main(){}";
    base=vk_shader_prepare_base(compute,STAGE_COMPUTE,why,sizeof why);assert(base);
    ShaderPrepareVariant cv={0};cv.key=source_key(compute,STAGE_COMPUTE);
    assert(vk_shader_prepare_variant(base,&cv,why,sizeof why));cv.kinds[0]=1;assert(!vk_shader_prepare_variant(base,&cv,why,sizeof why));
    vk_shader_prepare_base_free(base);vk_shader_prepare_end();uint32_t *spv=cache_load(cv.key,&n);assert(spv);free(spv);
    unsigned before=compiled;assert(!remove(base_file));pthread_t workers[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&workers[i],NULL,shared_base,NULL));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],NULL));
    assert(compiled==before+1);readable();
    // A failed atomic replacement is reported; a later preparation can repair it.
    assert(!remove(variant_file) && !mkdir(variant_file,0755));begin();
    base=vk_shader_prepare_base(sources[0],STAGE_VERTEX,why,sizeof why);assert(base);
    assert(!vk_shader_prepare_variant(base,&variants[1],why,sizeof why));
    vk_shader_prepare_base_free(base);vk_shader_prepare_end();assert(!rmdir(variant_file));prepare();readable();
    puts("PASS: shared base, dictionary roundtrip/resume, dependency and write-failure repair, v2 rejection, cross-thread/session isolation and concurrent base deduplication");
}
