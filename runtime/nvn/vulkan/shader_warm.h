// Observed-state module preparation. This does not certify an entire game or
// build a driver pipeline. Caller keeps game/profile settings stable until return.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "../maxwell/shader.h"
#ifdef __cplusplus
extern "C" {
#endif
bool vk_shader_warm_game(const char *game,const char *manifest,bool title_only,
                         void (*progress)(double,const char*),char *why,size_t capacity);
void vk_shader_translate_begin(void);
void vk_shader_translate_end(void);
bool vk_shader_prepare_compute_linear(void);
typedef struct {bool running,title_ready,failed;double progress;} ShaderWarmStatus;
bool vk_shader_warm_enabled(void);
void vk_shader_warm_start(const char *game,const char *manifest);
void vk_shader_warm_stop(void); // cancel and join, retaining completed cache entries
void vk_shader_warm_cancel_async(void); // lifecycle notification: request cancellation without joining
void vk_shader_warm_resume(void); // foreground resume stays low priority while gameplay is active
ShaderWarmStatus vk_shader_warm_status(void);
struct NvnShaderStage;
void vk_shader_warm_observe_begin(const char *game);
void vk_shader_warm_observe_flush(void);
void vk_shader_warm_observe_end(void);
void vk_shader_warm_observe(const struct NvnShaderStage *first,const struct NvnShaderStage *second,
                            const ShaderKey *first_key,const ShaderKey *second_key);
#ifdef __cplusplus
}
#endif
