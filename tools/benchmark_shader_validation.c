// Compare full default validation with numeric diagnostics on the same module.
// Run in dev, or build with the Android NDK and use the APK's shaderc library.
#include <spirv-tools/libspirv.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static spv_context (*context_create)(spv_target_env);
static void (*context_destroy)(spv_context);
static spv_validator_options (*options_create)(void);
static void (*options_destroy)(spv_validator_options);
static void (*friendly_names)(spv_validator_options, bool);
static spv_result_t (*validate_default)(spv_const_context, const uint32_t *, size_t, spv_diagnostic *);
static spv_result_t (*validate_options)(spv_const_context, spv_const_validator_options,
                                      spv_const_binary, spv_diagnostic *);
static void (*diagnostic_destroy)(spv_diagnostic);
static char last_error[8192];

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static spv_result_t validate(uint32_t *words, size_t count, bool numeric) {
    spv_context context = context_create(SPV_ENV_VULKAN_1_3);
    spv_validator_options options = numeric ? options_create() : NULL;
    if (!context || (numeric && !options)) exit(2);
    spv_diagnostic diagnostic = NULL;
    spv_result_t result;
    if (numeric) {
        friendly_names(options, false);
        spv_const_binary_t binary = {words, count};
        result = validate_options(context, options, &binary, &diagnostic);
    } else {
        result = validate_default(context, words, count, &diagnostic);
    }
    snprintf(last_error, sizeof last_error, "%s", diagnostic ? diagnostic->error : "");
    diagnostic_destroy(diagnostic);
    if (options) options_destroy(options);
    context_destroy(context);
    return result;
}

#define LOAD(variable, name) do { \
    *(void **)(&variable) = dlsym(library, name); \
    if (!variable) { fprintf(stderr, "missing %s: %s\n", name, dlerror()); return 2; } \
} while (0)

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: benchmark_shader_validation LIBRARY MODULE.spv [REPEATS]\n");
        return 2;
    }
    char *end = NULL;
    long repeats = argc == 4 ? strtol(argv[3], &end, 10) : 3;
    if (repeats < 1 || repeats > 1000 || (end && *end)) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    LOAD(context_create, "spvContextCreate");
    LOAD(context_destroy, "spvContextDestroy");
    LOAD(options_create, "spvValidatorOptionsCreate");
    LOAD(options_destroy, "spvValidatorOptionsDestroy");
    LOAD(friendly_names, "spvValidatorOptionsSetFriendlyNames");
    LOAD(validate_default, "spvValidateBinary");
    LOAD(validate_options, "spvValidateWithOptions");
    LOAD(diagnostic_destroy, "spvDiagnosticDestroy");
    FILE *file = fopen(argv[2], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 2;
    long size = ftell(file);
    if (size < 20 || size % 4 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) return 2;
    uint32_t *words = malloc(size), *original = malloc(size);
    if (!words || !original || fread(words, 1, size, file) != (size_t)size) return 2;
    fclose(file);
    memcpy(original, words, size);
    size_t count = size / 4;
    double elapsed[2] = {0, 0};
    // Alternate order to reduce warm-cache and frequency bias.
    for (long iteration = 0; iteration < repeats; ++iteration) {
        for (unsigned order = 0; order < 2; ++order) {
            unsigned numeric = (iteration + order) & 1;
            double started = seconds();
            spv_result_t result = validate(words, count, numeric);
            elapsed[numeric] += seconds() - started;
            if (result != SPV_SUCCESS || memcmp(words, original, size)) {
                fprintf(stderr, "validation/input mismatch mode=%u status=%d: %s\n", numeric, result, last_error);
                return 1;
            }
        }
    }
    unsigned negative_checks = 0;
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        memcpy(words, original, size);
        size_t test_count = count;
        if (mutation == 0) words[0] = 0;
        if (mutation == 1) words[3] = 1;
        if (mutation == 2) --test_count;
        spv_result_t normal = validate(words, test_count, false);
        spv_result_t numeric = validate(words, test_count, true);
        if (normal == SPV_SUCCESS || numeric != normal) {
            fprintf(stderr, "invalid-module mismatch mutation=%u status=%d/%d\n", mutation, normal, numeric);
            return 1;
        }
        ++negative_checks;
    }
    printf("{\"vulkan_minor\":3,\"words\":%zu,\"repeats\":%ld,\"default_seconds\":%.6f,\"numeric_seconds\":%.6f,"
           "\"speedup\":%.3f,\"input_unchanged\":true,\"negative_checks\":%u}\n",
           count, repeats, elapsed[0], elapsed[1], elapsed[0] / elapsed[1], negative_checks);
    free(words);
    free(original);
    dlclose(library);
    return 0;
}
