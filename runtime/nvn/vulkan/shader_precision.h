// Shared by the preparation compiler and its precision regressions.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// Position and explicit fragment depth must use the same arithmetic. TOTK also encodes material classes in depth:
// a fragment shader writes the class, then a lighting vertex shader computes it again for an EQUAL test. Allowing
// contraction only in the fragment stage changes water's D16 class from 39192 to 39193 on Adreno, rejecting its
// entire lighting pass. GLSL's precise qualifier cannot trace through the translated integer register/bitcast code.
static bool writes_fragment_depth(const uint32_t *spv, size_t nwords) {
    // OpExecutionMode DepthReplacing: inspect the compiled entry point, not source text or a game shader address.
    for (size_t i = 5; i < nwords;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || wc > nwords - i) return false;
        if (op == 16 && wc >= 3 && spv[i + 2] == 12) return true;
        i += wc;
    }
    return false;
}

static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    if (!done) { free(spv); *nwords = 0; return NULL; }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    if (!out) { free(done); free(spv); *nwords = 0; return NULL; }
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

