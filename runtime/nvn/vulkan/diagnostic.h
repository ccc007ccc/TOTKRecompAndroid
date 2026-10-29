// Bounded diagnostic snapshots, not a complete GPU replay capture.
#pragma once
#include "vk.h"
#include <stdbool.h>
#include <stdint.h>
#include "../../diagnostics_build.h"
typedef struct VkDiagnostic VkDiagnostic;
#if TOTK_DIAGNOSTICS
VkDiagnostic *vk_diag_create(void);
void vk_diag_destroy(VkDiagnostic *d); // GPU idle / capture readback complete
void vk_diag_begin(VkDiagnostic *d);
void vk_diag_end(VkDiagnostic *d); // after waiting for the frame's readback
void vk_diag_cancel(void);
bool vk_diag_active(void);
bool vk_diag_dirty(void);
void vk_diag_mark_dirty(void);
void vk_diag_next_group(void);
unsigned vk_diag_group(void);
// Only normalized/float color formats may be blitted to the float/UNORM previews.
bool vk_diag_float_color(VkFormat format);
void vk_diag_image(TexBackend *image, uint64_t address, unsigned slot,
                   unsigned level, unsigned layer, VkImageAspectFlags aspect);
void vk_diag_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
// CPU mirrors only. The record states both the original and captured byte counts.
unsigned vk_diag_blob(unsigned kind, uint64_t address, const void *data,
                      uint64_t size, unsigned limit);
bool vk_diag_write(VkDiagnostic *d, const char *directory, uint64_t *bytes);
uint64_t vk_diag_raw_bytes(const VkDiagnostic *d);
uint64_t vk_diag_pending_seq(const VkDiagnostic *d);
#else
static inline void vk_diag_cancel(void) {}
static inline bool vk_diag_active(void) { return false; }
static inline bool vk_diag_dirty(void) { return false; }
static inline void vk_diag_mark_dirty(void) {}
static inline void vk_diag_next_group(void) {}
static inline unsigned vk_diag_group(void) { return 0; }
static inline void vk_diag_image(TexBackend *image, uint64_t address, unsigned slot,
                                 unsigned level, unsigned layer, VkImageAspectFlags aspect) {
    (void)image; (void)address; (void)slot; (void)level; (void)layer; (void)aspect;
}
static inline void vk_diag_log(const char *format, ...) { (void)format; }
static inline unsigned vk_diag_blob(unsigned kind, uint64_t address, const void *data, uint64_t size, unsigned limit) {
    (void)kind; (void)address; (void)data; (void)size; (void)limit; return 0;
}
#endif
