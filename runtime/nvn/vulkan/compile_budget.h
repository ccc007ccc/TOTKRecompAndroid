// One admission budget for shaderc, interface derivation and Vulkan driver
// compilation. Acquire only around work, never around cache/dependency waits.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*VkCompileBudgetCheck)(void *);
typedef struct {VkCompileBudgetCheck check;void *context;} VkCompileBudgetDependency;
typedef struct { unsigned active, background, waiting, peak, limit; } VkCompileBudgetStats;
bool vk_compile_budget_background(bool background); // returns previous TLS mode
bool vk_compile_budget_is_background(void);
bool vk_compile_budget_cancelled(void);
VkCompileBudgetDependency vk_compile_budget_dependency(VkCompileBudgetDependency dependency);
void vk_compile_budget_checks(VkCompileBudgetCheck cancelled, VkCompileBudgetCheck urgent, void *context);
void vk_compile_budget_loading(bool loading);
void vk_compile_budget_wake(void);
bool vk_compile_budget_acquire(void); // false for a task-local preparation cancellation
void vk_compile_budget_release(void);
unsigned vk_compile_budget_workers(void);
VkCompileBudgetStats vk_compile_budget_stats(void);
#ifdef __cplusplus
}
#endif
