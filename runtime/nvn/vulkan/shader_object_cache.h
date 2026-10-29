// Render-thread-owned lookup tables for shader programs and graphics pipelines.
// Values keep their original ownership; growing/clearing only changes the index.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t key; void *object; } ShaderObjectEntry;
typedef struct {
    ShaderObjectEntry *entries;
    size_t capacity, count, initial_capacity;
} ShaderObjectTable;

static inline size_t shader_object_slot(const ShaderObjectTable *table, uint64_t key) {
    size_t slot = (size_t)(key ^ (key >> 31)) & (table->capacity - 1);
    while (table->entries[slot].key && table->entries[slot].key != key)
        slot = (slot + 1) & (table->capacity - 1);
    return slot;
}
static inline void *shader_object_find(const ShaderObjectTable *table, uint64_t key) {
    if (!table->capacity || !key) return NULL;
    return table->entries[shader_object_slot(table, key)].object;
}
static inline bool shader_object_put(ShaderObjectTable *table, uint64_t key, void *object) {
    if (!key || !object) return false;
    if (table->capacity) {
        size_t slot = shader_object_slot(table, key);
        if (table->entries[slot].key) { table->entries[slot].object = object; return true; }
    }
    // Retain empty buckets even after visiting more combinations than the old
    // fixed limits. Otherwise an absent-key lookup would loop forever.
    if (!table->capacity || table->count >= table->capacity / 2) {
        if (table->capacity > SIZE_MAX / 2 / sizeof *table->entries) return false;
        size_t capacity = table->capacity ? table->capacity * 2 : table->initial_capacity;
        if (capacity < 2 || (capacity & (capacity - 1)) || capacity > SIZE_MAX / sizeof *table->entries) return false;
        ShaderObjectEntry *entries = (ShaderObjectEntry *)calloc(capacity, sizeof *entries);
        if (!entries) return false;
        ShaderObjectEntry *old = table->entries;
        size_t previous = table->capacity;
        table->entries = entries; table->capacity = capacity;
        for (size_t i = 0; i < previous; ++i)
            if (old[i].key) entries[shader_object_slot(table, old[i].key)] = old[i];
        free(old);
    }
    table->entries[shader_object_slot(table, key)] = (ShaderObjectEntry){key, object};
    ++table->count;
    return true;
}
static inline void shader_object_clear(ShaderObjectTable *table) {
    if (table->entries) memset(table->entries, 0, table->capacity * sizeof *table->entries);
    table->count = 0;
}
