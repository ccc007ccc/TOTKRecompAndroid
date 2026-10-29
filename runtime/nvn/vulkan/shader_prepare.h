// Explicit and background preparation share the cache with gameplay fallback.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool vk_shader_prepare_begin(char *why, size_t capacity);
void vk_shader_prepare_end(void);
bool vk_shader_prepare_source(const char *source, int stage, const char *name, char *why, size_t capacity);
// One all-float module per source/state, shared by every interface variant.
// Handles belong to the calling thread's preparation session and must be freed.
typedef struct ShaderPrepareBase ShaderPrepareBase;
typedef struct { uint64_t key; uint8_t kinds[32]; } ShaderPrepareVariant;
ShaderPrepareBase *vk_shader_prepare_base(const char *source, int stage, char *why, size_t capacity);
void vk_shader_prepare_base_free(ShaderPrepareBase *base);
bool vk_shader_prepare_variant(ShaderPrepareBase *base, const ShaderPrepareVariant *variant, char *why, size_t capacity);
#define SHADER_PREPARE_CACHE_VERSION 3
uint64_t vk_shader_prepare_key(const char *source, int stage);
// Verify a complete cached module by content key without loading the compiler.
bool vk_shader_cached(uint64_t key);
// Read-only audit access; uses exactly the same bounded decoder as gameplay.
uint32_t *vk_shader_cache_load_key(uint64_t key,bool base,size_t *words);
const char *vk_shader_cache_namespace(void);
typedef struct {uint64_t current_hits,compatible_hits,misses;} ShaderCacheStats;
// Cumulative lookup counts for this process, excluding recursive dictionary reads.
ShaderCacheStats vk_shader_cache_stats(void);
uint64_t vk_shader_prepare_profile(void);
void vk_shader_prepare_configure(void);
unsigned vk_shader_prepare_gather(void);
bool vk_shader_prepare_vertex_snap(void);
bool vk_shader_prepare_game(const char *game, void (*progress)(double, const char *), char *why, size_t capacity);
// Cheap UI metadata query; never proves module completeness.
bool vk_shader_game_has_receipt(const char *game);
// Full blocking audit for tools/setup workers. UI uses the cancellable job below.
bool vk_shader_game_ready(const char *game);
typedef struct {bool running,ready,failed;double progress;char message[256];} ShaderCheckStatus;
void vk_shader_check_start(const char *game);
void vk_shader_check_stop(void);
ShaderCheckStatus vk_shader_check_status(void);
const char *vk_builtin_source(const char *name, int *stage);
// Private compiler plugin ABI. Allocations are released by the caller with free().
typedef uint32_t *(*ShaderPrepareCompile)(const char *, int, unsigned, bool, size_t *, char *, size_t);
typedef void *(*ShaderPrepareCreate)(void);
typedef void (*ShaderPrepareDestroy)(void *);
typedef uint32_t *(*ShaderPrepareCompileContext)(void *, const char *, int, unsigned, bool, size_t *, char *, size_t);
typedef uint32_t *(*ShaderPrepareDerive)(const uint32_t *, size_t, int, const uint8_t *, unsigned, size_t *, char *, size_t);
#ifdef __cplusplus
}
#endif
