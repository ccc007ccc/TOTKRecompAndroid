// SPIR-V cache loading, optional gameplay compilation, reflection, and built-in modules.
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#define RTLD_NOW 0
#define RTLD_LOCAL 0
static void *dlopen(const char *path, int flags) { (void)flags; return (void *)LoadLibraryA(path); }
static void *dlsym(void *library, const char *name) { return (void *)GetProcAddress((HMODULE)library, name); }
static void dlclose(void *library) { FreeLibrary((HMODULE)library); }
static const char *dlerror(void) { return "cannot load shader preparation plugin/export"; }
#else
#include <dlfcn.h>
#endif
#ifndef RTLD_NODELETE
#define RTLD_NODELETE 0
#endif
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <zstd.h>
#include "shaders.h"
#include "compile_budget.h"
#include "shader_cache.h"
#include "shader_source_key.h"
#include "shader_prepare.h"
#include "shader_runtime.h"
#include "deswizzle_shader.h"
#ifdef __ANDROID__
#include "../../settings.h"
#endif

#ifndef SHADER_BUILD_ID
#define SHADER_BUILD_ID "development"
#endif
#ifdef HAVE_SHADER_CACHE_CONTRACT
#include "shader_cache_contract.h"
#endif
#ifndef SHADER_CACHE_ID
#define SHADER_CACHE_ID SHADER_BUILD_ID
#endif
#ifndef SHADER_COMPAT_CACHE_IDS
#define SHADER_COMPAT_CACHE_IDS
#endif
static const char* const cache_namespaces[] = {SHADER_CACHE_ID, SHADER_BUILD_ID, SHADER_COMPAT_CACHE_IDS NULL};
static _Atomic uint64_t cache_current_hits,cache_compatible_hits,cache_misses;
const char *vk_shader_cache_namespace(void) { return SHADER_CACHE_ID; }
ShaderCacheStats vk_shader_cache_stats(void) { return (ShaderCacheStats){cache_current_hits,cache_compatible_hits,cache_misses}; }
#define shader_api vk_shader_target_api
#define source_key vk_shader_source_key
uint64_t vk_shader_prepare_key(const char *src, int stage) { return source_key(src, stage); }
static void cache_path(char *path, size_t size, uint64_t h) {
    snprintf(path, size, "cache/shaders/" SHADER_CACHE_ID "/%016llx.bin", (unsigned long long)h);
}
static void base_path(char *path, size_t size, uint64_t h) {
    snprintf(path, size, "cache/shaders/" SHADER_CACHE_ID "/%016llx.base.bin", (unsigned long long)h);
}
// v3: standalone Zstd frames or complete modules compressed against a base.
// Bases have their own namespace and MUST be standalone: no dependency chains.
typedef struct { uint64_t magic, key, words, digest, stored_bytes, encoding, base_key, base_digest; } ShaderEnvelope;
#define SHADER_ENVELOPE_MAGIC 0x335650534b544f54ull
enum { SHADER_STANDALONE = 0, SHADER_DICTIONARY = 1 };
struct ShaderPrepareBase {
    uint64_t key, digest, session;
    const char *cache_namespace;
    uint32_t *spv;
    size_t words;
    int stage;
    unsigned api;
    bool optimized;
    pthread_t owner;
    ZSTD_CDict *dictionary;
    ZSTD_CCtx *compressor;
};
static uint64_t payload_hash(const uint32_t *data, size_t words) {
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < words * 4; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static uint32_t *cache_load_at(const char* cache_namespace,uint64_t h, size_t *nwords, bool is_base, const ShaderPrepareBase *base) {
    *nwords = 0;
    char path[192];
    snprintf(path,sizeof path,"cache/shaders/%s/%016llx%s.bin",cache_namespace,(unsigned long long)h,is_base?".base":"");
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    ShaderEnvelope header;
    if (n <= (long)sizeof header || fread(&header, sizeof header, 1, f) != 1 ||
        header.magic != SHADER_ENVELOPE_MAGIC || header.key != h || header.words <= 5 ||
        header.words > 4 * 1024 * 1024 || header.stored_bytes != (size_t)n - sizeof header ||
        header.stored_bytes > ZSTD_compressBound(header.words * 4) ||
        header.encoding > SHADER_DICTIONARY || (is_base && header.encoding != SHADER_STANDALONE) ||
        (header.encoding == SHADER_STANDALONE && (header.base_key || header.base_digest))) { fclose(f); return NULL; }
    size_t bytes = header.words * 4;
    uint32_t *d = malloc(bytes);
    void *packed = malloc(header.stored_bytes);
    bool read_ok = packed && fread(packed, 1, header.stored_bytes, f) == header.stored_bytes;
    fclose(f);
    bool ok = d && read_ok && ZSTD_getFrameContentSize(packed, header.stored_bytes) == bytes &&
              ZSTD_findFrameCompressedSize(packed, header.stored_bytes) == header.stored_bytes;
    if (ok && header.encoding == SHADER_DICTIONARY) {
        // Preparation can reuse the current base in memory. Gameplay reads the
        // referenced standalone base, verifies its digest, then only decompresses.
        size_t words = 0; uint32_t *owned = NULL;
        const uint32_t *dict = NULL;
        bool digest_ok;
        if (base && base->cache_namespace && !strcmp(base->cache_namespace,cache_namespace) &&
            base->key == header.base_key && base->digest == header.base_digest) {
            dict = base->spv; words = base->words; digest_ok = true;
        } else {
            // Resolve a dictionary in the same namespace as its module. A base
            // in another compatible directory cannot repair an incomplete pair.
            owned = cache_load_at(cache_namespace,header.base_key, &words, true, NULL); dict = owned;
            digest_ok = dict && payload_hash(dict, words) == header.base_digest;
        }
        ZSTD_DCtx *context = ZSTD_createDCtx();
        ok = digest_ok && context &&
             ZSTD_decompress_usingDict(context, d, bytes, packed, header.stored_bytes, dict, words * 4) == bytes;
        ZSTD_freeDCtx(context); free(owned);
    } else if (ok) ok = ZSTD_decompress(d, bytes, packed, header.stored_bytes) == bytes;
    if (!ok) { free(d); d = NULL; }
    free(packed);
    if (d && (!vk_shader_cache_framed(d, header.words) || payload_hash(d, header.words) != header.digest)) { free(d); d = NULL; }
    if (d) *nwords = header.words;
    return d;
}
static uint32_t *cache_load_namespace(uint64_t h,size_t *nwords,bool is_base,
                                      const ShaderPrepareBase *base,const char **loaded_namespace) {
    if (loaded_namespace) *loaded_namespace = NULL;
    for (unsigned i=0; cache_namespaces[i]; ++i) {
        bool duplicate = false;
        for (unsigned j=0; j<i; ++j) if (!strcmp(cache_namespaces[i],cache_namespaces[j])) duplicate = true;
        if (duplicate) continue;
        uint32_t *words = cache_load_at(cache_namespaces[i],h,nwords,is_base,base);
        if (words) {
            if (i) ++cache_compatible_hits; else ++cache_current_hits;
            if (loaded_namespace) *loaded_namespace = cache_namespaces[i];
            return words;
        }
    }
    ++cache_misses;
    return NULL;
}
static uint32_t *cache_load_module(uint64_t h,size_t *nwords,bool is_base,const ShaderPrepareBase *base) {
    return cache_load_namespace(h,nwords,is_base,base,NULL);
}
uint32_t *vk_shader_cache_load_key(uint64_t h,bool base,size_t* words){return cache_load_module(h,words,base,NULL);}
static uint32_t *cache_load(uint64_t h, size_t *nwords) { return cache_load_module(h, nwords, false, NULL); }
bool vk_shader_cached(uint64_t h) {
    size_t words=0;uint32_t* spv=cache_load(h,&words);bool valid=spv!=NULL;free(spv);return valid;
}
static bool cache_store_module(uint64_t h, const uint32_t *spv, size_t nwords, bool is_base, ShaderPrepareBase *base) {
    if (!vk_shader_cache_framed(spv, nwords) || (is_base && base)) return false;
    // A new variant must have its dictionary beside it, even when preparation
    // reused a legacy base. Existing complete legacy hits stay read-only.
    if (base && (!base->cache_namespace || strcmp(base->cache_namespace,SHADER_CACHE_ID))) {
        if (!cache_store_module(base->key,base->spv,base->words,true,NULL)) return false;
        base->cache_namespace = SHADER_CACHE_ID;
    }
    size_t capacity = ZSTD_compressBound(nwords * 4);
    void *packed = malloc(capacity); if (!packed) return false;
    size_t stored = base ? ZSTD_compress_usingCDict(base->compressor, packed, capacity, spv, nwords * 4, base->dictionary) :
                          ZSTD_compress(packed, capacity, spv, nwords * 4, 3);
    if (ZSTD_isError(stored)) { free(packed); return false; }
    mkdir("cache", 0755); mkdir("cache/shaders", 0755); mkdir("cache/shaders/" SHADER_CACHE_ID, 0755);
    char path[192], tmp[224];
    if (is_base) base_path(path, sizeof path, h); else cache_path(path, sizeof path, h);
    snprintf(tmp, sizeof tmp, "%s.%llx.part", path, (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb"); if (!f) { free(packed); return false; }
    ShaderEnvelope header = {SHADER_ENVELOPE_MAGIC, h, nwords, payload_hash(spv, nwords), stored,
        base ? SHADER_DICTIONARY : SHADER_STANDALONE, base ? base->key : 0, base ? base->digest : 0};
    bool ok = fwrite(&header, sizeof header, 1, f) == 1 && fwrite(packed, 1, stored, f) == stored;
    free(packed);
    if (fclose(f)) ok = false;
    if (ok && !rename(tmp, path)) return true;
    remove(tmp); return false;
}
static bool cache_store(uint64_t h, const uint32_t *spv, size_t nwords) { return cache_store_module(h, spv, nwords, false, NULL); }
// A preparation session belongs to its calling thread. Other threads never gain
// compiler access, even while the launcher is displaying preparation progress.
static _Thread_local void *prepare_library;
static _Thread_local void *prepare_context;
static _Thread_local ShaderPrepareCompileContext prepare_compile;
static uint32_t *budget_compile(const char *src,int stage,unsigned api,bool optimized,size_t *words,char *why,size_t capacity) {
    if(!vk_compile_budget_acquire()){snprintf(why,capacity,"graphics compilation cancelled");return NULL;}
    uint32_t *result=prepare_compile(prepare_context,src,stage,api,optimized,words,why,capacity);
    vk_compile_budget_release();return result;
}
static _Thread_local ShaderPrepareDestroy prepare_destroy;
static _Thread_local ShaderPrepareDerive prepare_derive;
static _Thread_local uint64_t prepare_session;
bool vk_shader_prepare_begin(char *why, size_t capacity) {
    if (prepare_library) { snprintf(why, capacity, "shader preparation already active"); return false; }
    const char *dir = getenv("LARECOMP_NATIVE_LIB_DIR");
    char path[1200]; snprintf(path, sizeof path, "%s%slibshaderprepare.so", dir ? dir : "", dir ? "/" : "");
    // shaderc intentionally retains a process-lifetime initialization mutex.
    // Unmapping/reloading it loses that mutex on each preparation session.
    // Keep the library mapping, while closing every compiler context and entry
    // point below. Gameplay opens a session only for an enabled cache-miss fallback.
    prepare_library = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    ShaderPrepareCreate create = NULL;
    if (prepare_library) {
        prepare_compile = (ShaderPrepareCompileContext)dlsym(prepare_library, "shader_prepare_compile_context");
        prepare_destroy = (ShaderPrepareDestroy)dlsym(prepare_library, "shader_prepare_destroy");
        prepare_derive = (ShaderPrepareDerive)dlsym(prepare_library, "shader_prepare_derive");
        create = (ShaderPrepareCreate)dlsym(prepare_library, "shader_prepare_create");
    }
    if (!prepare_compile || !prepare_destroy || !prepare_derive || !create) {
        const char *error = dlerror(); snprintf(why, capacity, "shader preparation tool unavailable: %s", error ? error : path);
        vk_shader_prepare_end(); return false;
    }
    prepare_context = create();
    if (!prepare_context) { snprintf(why, capacity, "cannot allocate shader preparation compiler"); vk_shader_prepare_end(); return false; }
    ++prepare_session;
    return true;
}
void vk_shader_prepare_end(void) {
    prepare_compile = NULL; prepare_derive = NULL;
    if (prepare_destroy && prepare_context) prepare_destroy(prepare_context);
    prepare_context = NULL; prepare_destroy = NULL;
    if (prepare_library) dlclose(prepare_library);
    prepare_library = NULL;
}
static pthread_once_t base_locks_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t base_locks[64];
static void init_base_locks(void) {
    for (size_t i = 0; i < sizeof base_locks / sizeof *base_locks; ++i) pthread_mutex_init(&base_locks[i], NULL);
}
bool vk_shader_prepare_source(const char *src, int stage, const char *name, char *why, size_t capacity) {
    if (!prepare_compile) { snprintf(why, capacity, "shader compiler is closed outside preparation"); return false; }
    size_t n = 0; uint64_t h = source_key(src, stage);
    pthread_once(&base_locks_once,init_base_locks);
    pthread_mutex_t *lock=&base_locks[h % (sizeof base_locks/sizeof *base_locks)];
    pthread_mutex_lock(lock);
    uint32_t *spv = cache_load(h, &n);
    if (spv) { free(spv);pthread_mutex_unlock(lock);return true; }
    spv = budget_compile(src, stage, shader_api(), getenv("LARECOMP_VK_OPT") != NULL, &n, why, capacity);
    if (!spv) { LOG("[shader-prepare] %s: %s\n", name, why);pthread_mutex_unlock(lock);return false; }
    bool ok = vk_shader_cache_framed(spv, n) && cache_store(h, spv, n);
    free(spv);
    pthread_mutex_unlock(lock);
    if (!ok) snprintf(why, capacity, "cannot save prepared shader %s (check free storage)", name);
    return ok;
}
// Identical canonical sources from different records may reach different workers.
// Serialize their cache miss/compile/store, without retaining an unbounded set of
// modules in RAM or serializing all shaderc work behind one lock.
void vk_shader_prepare_base_free(ShaderPrepareBase *base) {
    if (!base) return;
    ZSTD_freeCDict(base->dictionary); ZSTD_freeCCtx(base->compressor); free(base->spv); free(base);
}
ShaderPrepareBase *vk_shader_prepare_base(const char *src, int stage, char *why, size_t capacity) {
    if (!prepare_compile) { snprintf(why, capacity, "shader compiler is closed outside preparation"); return NULL; }
    if (!src || (stage != STAGE_VERTEX && stage != STAGE_FRAGMENT && stage != STAGE_COMPUTE)) {
        snprintf(why, capacity, "invalid base shader source/stage"); return NULL;
    }
    ShaderPrepareBase *base = calloc(1, sizeof *base);
    if (!base) { snprintf(why, capacity, "cannot allocate base shader"); return NULL; }
    base->key = source_key(src, stage); base->stage = stage; base->api = shader_api();
    base->optimized = getenv("LARECOMP_VK_OPT") != NULL; base->session = prepare_session; base->owner = pthread_self();
    pthread_once(&base_locks_once, init_base_locks);
    pthread_mutex_t *lock = &base_locks[base->key % (sizeof base_locks / sizeof *base_locks)];
    pthread_mutex_lock(lock);
    base->spv = cache_load_namespace(base->key, &base->words, true, NULL, &base->cache_namespace);
    if (!base->spv) {
        base->spv = budget_compile(src, stage, base->api, base->optimized, &base->words, why, capacity);
        if (base->spv && !cache_store_module(base->key, base->spv, base->words, true, NULL)) {
            snprintf(why, capacity, "cannot save prepared base shader (check free storage)");
            free(base->spv); base->spv = NULL;
        }
        if (base->spv) base->cache_namespace = SHADER_CACHE_ID;
    }
    pthread_mutex_unlock(lock);
    if (!base->spv) { vk_shader_prepare_base_free(base); return NULL; }
    base->digest = payload_hash(base->spv, base->words);
    base->dictionary = ZSTD_createCDict(base->spv, base->words * 4, 3);
    base->compressor = ZSTD_createCCtx();
    if (!base->dictionary || !base->compressor) {
        snprintf(why, capacity, "cannot allocate shader dictionary compressor"); vk_shader_prepare_base_free(base); return NULL;
    }
    return base;
}
bool vk_shader_prepare_variant(ShaderPrepareBase *base, const ShaderPrepareVariant *variant, char *why, size_t capacity) {
    if (!prepare_derive) { snprintf(why, capacity, "shader interface preparation is closed"); return false; }
    if (!base || !variant || !pthread_equal(base->owner, pthread_self()) || base->session != prepare_session ||
        base->api != shader_api() || base->optimized != (getenv("LARECOMP_VK_OPT") != NULL)) {
        snprintf(why, capacity, "shader base belongs to a different preparation session/profile"); return false;
    }
    bool changed = false;
    for (unsigned i = 0; i < 32; ++i) {
        if (variant->kinds[i] > 2 || (variant->kinds[i] && (base->stage == STAGE_COMPUTE || (base->stage == STAGE_FRAGMENT && i >= 8)))) {
            snprintf(why, capacity, "invalid shader interface kind/location"); return false;
        }
        changed |= variant->kinds[i] != 0;
    }
    size_t n = 0;
    uint32_t *spv = cache_load_module(variant->key, &n, false, base);
    if (spv) { free(spv); return true; }
    // The all-float (and compute) module has already passed the compiler's validator.
    if(changed && !vk_compile_budget_acquire()){snprintf(why,capacity,"graphics compilation cancelled");return false;}
    spv = changed ? prepare_derive(base->spv, base->words, base->stage, variant->kinds, base->api, &n, why, capacity) : base->spv;
    if(changed)vk_compile_budget_release();
    if (!spv) return false;
    if (!changed) n = base->words;
    bool ok = cache_store_module(variant->key, spv, n, false, base);
    if (changed) free(spv);
    if (!ok) snprintf(why, capacity, "cannot save prepared shader variant (check free storage)");
    return ok;
}
uint32_t *vk_load_prepared_shader(const char *src, int stage, const char *name, size_t *nwords) {
    *nwords = 0;
    uint64_t h = source_key(src, stage);
    uint32_t *spv = cache_load(h, nwords);
    if (!spv) FATAL("Prepared shader missing or damaged: %s (%016llx). Prepare graphics before playing.\n", name, (unsigned long long)h);
    return spv;
}

bool vk_shader_runtime_compile_enabled(void) {
    const char *value = getenv("LARECOMP_SHADER_RUNTIME_COMPILE");
    if (value) return atoi(value) != 0;
#ifdef __ANDROID__
    return g_settings.shader_runtime_compile;
#else
    return true;
#endif
}

uint32_t *vk_load_shader(const char *src, int stage, const char *name, size_t *nwords) {
    if (!vk_shader_runtime_compile_enabled()) return vk_load_prepared_shader(src, stage, name, nwords);
    uint64_t h = source_key(src, stage);
    uint32_t *spv = cache_load(h, nwords);
    if (spv) return spv;
    // Workers may request the same source concurrently. Recheck after taking a
    // source lock so only one compiles it; unrelated sources can build in parallel.
    pthread_once(&base_locks_once, init_base_locks);
    pthread_mutex_t *lock = &base_locks[h % (sizeof base_locks / sizeof *base_locks)];
    pthread_mutex_lock(lock);
    spv = cache_load(h, nwords);
    char why[1024] = "";
    if (!spv) {
        bool opened = !prepare_library;
        if (!opened || vk_shader_prepare_begin(why, sizeof why)) {
            LOG("[shader-runtime] compiling %s (%016llx)\n", name, (unsigned long long)h);
            spv = budget_compile(src, stage, shader_api(), getenv("LARECOMP_VK_OPT") != NULL,
                                  nwords, why, sizeof why);
            if (spv && !vk_shader_cache_framed(spv, *nwords)) {
                free(spv); spv = NULL; *nwords = 0;
                snprintf(why, sizeof why, "compiler returned an invalid SPIR-V module");
            }
            if (spv && !cache_store(h, spv, *nwords))
                LOG("[shader-runtime] could not cache %s; using the compiled module\n", name);
            if (opened) vk_shader_prepare_end();
        }
    }
    pthread_mutex_unlock(lock);
    if (!spv) FATAL("Runtime shader compilation failed: %s (%016llx): %s\n", name, (unsigned long long)h, why);
    return spv;
}

static void prepare_builtin(const char *name, bool *opened) {
    int stage;
    const char *source = vk_builtin_source(name, &stage);
    if (!source) FATAL("Unknown built-in shader: %s\n", name);
    if (vk_shader_cached(source_key(source, stage))) return;
    char why[1024];
    if (!*opened) {
        if (!vk_shader_prepare_begin(why, sizeof why)) FATAL("%s\n", why);
        *opened = true;
    }
    if (!vk_shader_prepare_source(source, stage, name, why, sizeof why))
        FATAL("Built-in shader preparation: %s\n", why);
}
void vk_shaders_init(void) {
    // Runtime-owned shaders are also prepared before the guest starts. All clear
    // target/type combinations must be included, including the depth-only clear.
    static const char *names[] = {"present.vert", "present.frag", "present_a.frag", "post.vert",
        "smaa_edge.frag", "smaa_weight.frag", "smaa_blend.frag", "easu.frag", "rcas.frag",
        "sgsr1.frag", "clear.vert", "clear.frag.none", "deswizzle.comp"};
    bool opened = false;
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) prepare_builtin(names[i], &opened);
    for (int target = 0; target < 8; ++target) for (int kind = 0; kind < 3; ++kind) {
        char name[32]; snprintf(name, sizeof name, "clear.frag.%c%d", "fiu"[kind], target);
        prepare_builtin(name, &opened);
    }
    if (opened) vk_shader_prepare_end();
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    if(!vk_compile_budget_acquire())return VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(g_vk.dev, &ci, NULL, &m);
    vk_compile_budget_release();
    if (result != VK_SUCCESS) LOG("[shader] vkCreateShaderModule failed: %d (%zu words)\n", result, nwords);
    return result == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_module_from_prepared(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_load_prepared_shader(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
            if(o==OP_TYPE_IMAGE && wc>=9)sc[w[1]]=w[7]; // Sampled=2 denotes a storage image
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = sc[t]==2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkResult vk_try_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages, VkDescriptorSetLayout *layout) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    *layout=VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, layout);
}
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayout l;
    VK_CHECK(vk_try_set_layout(b,n,set,stages,&l));
    return l;
}

// ================================================================== built-in shaders
const char *vk_builtin_source(const char *name, int *stage) {
    if (!strcmp(name, "deswizzle.comp")) {  // (deswizzle.c upload conversion)
        *stage = STAGE_COMPUTE;
        return VK_DESWIZZLE_GLSL;
    }
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, uv).rgb, 1.0); }\n";
    }
    if (!strcmp(name, "present_a.frag")) {  // (debug pass dumps: alpha kept)
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = texture(t, uv); }\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size (alpha: bilinear, for the 3D scene's composite: exec.c scene_upscale). 12 taps around the
    // source pixel, direction / edge length from luma, Lanczos-2-like weights stretched along the edge, clamped to the
    // nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  float alpha = texture(t, (pp + 0.5) / pc.b.xy).a;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               // (weights summing to ~0 on a saturated patch: NaN, and min / max of a NaN may give black - a black dash
               // by a white rock in the scene at 50%; the nearest texel there)
               "  o = vec4(aw > 1.0 / 65536.0 ? min(mx, max(mn, ac / aw)) : fC, alpha);\n"
               "}\n";
    }
    // Snapdragon Game Super Resolution 1 (sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag of
    // github.com/SnapdragonStudios/snapdragon-gsr: Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights
    // reserved. SPDX-License-Identifier: BSD-3-Clause), its default mode (RGBA, luma from green, no edge direction):
    // one pass, a bilinear sample sharpened where the 12 texels around it show an edge (Lanczos-2-like weights from
    // their deviation from the mean). a = (1 / input size, input size), b.xy = output size. Alpha: the bilinear
    // sample's (the 3D scene's composite: exec.c scene_upscale), where SGSR writes 1. Its gathers and its bilinear
    // sample are texelFetch here (Vulkan's own rules: texel centers, (i0,j1) (i1,j1) (i1,j0) (i0,j0), clamp): on the 3D
    // scene's A2B10G10R10 image textureGather and the sampler gave Turnip (Adreno 830) a blurred, speckled image, the
    // same shader through the sampler was right on the RGBA8 final image.
    if (!strcmp(name, "sgsr1.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ps0;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "const float EdgeThreshold = 8.0 / 255.0, EdgeSharpness = 2.0;\n"
               "vec4 tx(ivec2 p) { return texelFetch(ps0, clamp(p, ivec2(0), ivec2(pc.a.zw) - 1), 0); }\n"
               "vec4 bilinear(vec2 uv) {\n"
               "  vec2 x = uv * pc.a.zw - 0.5, f = fract(x);\n"
               "  ivec2 i = ivec2(floor(x));\n"
               "  return mix(mix(tx(i), tx(i + ivec2(1, 0)), f.x), mix(tx(i + ivec2(0, 1)), tx(i + ivec2(1, 1)), f.x), f.y);\n"
               "}\n"
               "vec4 gatherG(vec2 uv) {\n"
               "  ivec2 i = ivec2(floor(uv * pc.a.zw - 0.5));\n"
               "  return vec4(tx(i + ivec2(0, 1)).g, tx(i + ivec2(1, 1)).g, tx(i + ivec2(1, 0)).g, tx(i).g);\n"
               "}\n"
               "float fastLanczos2(float x) { float wA = x - 4.0; float wB = x * wA - wA; wA *= wA; return wB * wA; }\n"
               "vec2 weightY(float dx, float dy, float c, float std) {\n"
               "  float x = (dx * dx + dy * dy) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);\n"
               "  float w = fastLanczos2(x);\n"
               "  return vec2(w, w * c);\n"
               "}\n"
               "void main() {\n"
               "  vec4 vi = pc.a;\n"
               "  vec2 uv = gl_FragCoord.xy / pc.b.xy;\n"
               "  vec4 color = bilinear(uv);\n"
               "  float alpha = color.w;\n"
               "  vec2 imgCoord = uv * vi.zw + vec2(-0.5, 0.5);\n"
               "  vec2 imgCoordPixel = floor(imgCoord);\n"
               "  vec2 coord = imgCoordPixel * vi.xy;\n"
               "  vec2 pl = imgCoord - imgCoordPixel;\n"
               "  vec4 left = gatherG(coord);\n"
               "  float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);\n"
               "  if (edgeVote > EdgeThreshold) {\n"
               "    coord.x += vi.x;\n"
               "    vec4 right = gatherG(coord + vec2(vi.x, 0.0));\n"
               "    vec4 upDown;\n"
               "    upDown.xy = gatherG(coord + vec2(0.0, -vi.y)).wz;\n"
               "    upDown.zw = gatherG(coord + vec2(0.0, vi.y)).yx;\n"
               "    float mean = (left.y + left.z + right.x + right.w) * 0.25;\n"
               "    left -= vec4(mean); right -= vec4(mean); upDown -= vec4(mean);\n"
               "    color.w = color.y - mean;\n"
               "    float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) + abs(right.x) + abs(right.y) +\n"
               "                abs(right.z) + abs(right.w) + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);\n"
               "    float sumMean = 1.014185e+01 / sum;\n"
               "    float std = sumMean * sumMean;\n"
               "    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);\n"
               "    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);\n"
               "    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);\n"
               "    aWY += weightY(pl.x, pl.y, left.z, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);\n"
               "    float finalY = aWY.y / aWY.x;\n"
               "    float maxY = max(max(left.y, left.z), max(right.x, right.w));\n"
               "    float minY = min(min(left.y, left.z), min(right.x, right.w));\n"
               "    float deltaY = clamp(EdgeSharpness * finalY, minY, maxY) - color.w;\n"
               "    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);\n"  // (smooth high contrast input)
               "    color.xyz = clamp(color.xyz + deltaY, 0.0, 1.0);\n"
               "  }\n"
               "  o = vec4(color.xyz, alpha);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), texelFetch(t, ip, 0).a);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = vk_builtin_source(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_module_from_prepared(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
