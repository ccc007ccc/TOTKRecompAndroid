#pragma once
#include <stddef.h>

// Android production builds fail closed. Gradle explicitly enables diagnostics
// for Debug and the independent performance-test package, regardless of profile.
// Desktop tools retain their existing opt-in diagnostic workflows.
#ifndef TOTK_DIAGNOSTICS
#ifdef __ANDROID__
#define TOTK_DIAGNOSTICS 0
#else
#define TOTK_DIAGNOSTICS 1
#endif
#endif

#if TOTK_DIAGNOSTICS
#include <stdlib.h>
#define DIAG_ENV(name) ENV(name)
#else
#define DIAG_ENV(name) ((const char *)NULL)
#endif

static inline const char *diag_getenv(const char *name) {
#if TOTK_DIAGNOSTICS
    return getenv(name);
#else
    (void)name;
    return NULL;
#endif
}
