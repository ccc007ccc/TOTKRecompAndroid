// Cache-first gameplay loading with an optional compiler fallback.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool vk_shader_runtime_compile_enabled(void);
uint32_t *vk_load_shader(const char *source, int stage, const char *name, size_t *nwords);
#ifdef __cplusplus
}
#endif
