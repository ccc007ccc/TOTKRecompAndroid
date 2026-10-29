"""Run the actual Vulkan index expansion with unaligned 8/16/32-bit inputs."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / '.work/totk-port/primitive-tests').resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('static uint32_t expand_index(')
end = source.index('\nstatic bool topology(', start)
unit = out / 'primitive-indices.c'
unit.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
static jmp_buf failed;
#define FATAL(...) longjmp(failed, 1)
''' + source[start:end] + r'''
static void check(uint32_t mode, const uint32_t *input, unsigned count, int type,
                  bool restart, uint32_t marker, const uint32_t *expected, unsigned n) {
    unsigned width = 1u << type;
    uint8_t *bytes = malloc((size_t)count * width + 1);
    assert(bytes);
    for (unsigned i = 0; i < count; ++i) memcpy(bytes + 1 + i * width, &input[i], width);
    uint32_t got;
    uint32_t *indices = expand_indices(mode, count, bytes + 1, type, 0, restart, marker, &got);
    assert(got == n && (!n || !memcmp(indices, expected, n * sizeof *indices)) && indices[n] == 0);
    free(indices); free(bytes);
}
int main(void) {
    const uint32_t vertices[] = {0,1,2,3,4,5,6};
    // Same winding as NVN's (0,1,3)/(2,0,3), with its last vertex first for Vulkan flat interpolation.
    const uint32_t strip[] = {3,0,1,3,2,0,5,2,3,5,4,2};
    const uint32_t split[] = {9,10,11,12,13,77,20,21,22,23,77,30,31,32};
    const uint32_t split_expected[] = {12,9,10,12,11,9,23,20,21,23,22,20};
    for (int type = 0; type < 3; ++type) {
        for (unsigned count = 0; count < 4; ++count) check(8, vertices, count, type, false, 0, NULL, 0);
        check(8, vertices, 4, type, false, 0, strip, 6);
        check(8, vertices, 6, type, false, 0, strip, 12);
        check(8, vertices, 7, type, false, 0, strip, 12); // incomplete tail
        check(8, split, 14, type, true, 77, split_expected, 12); // odd tails before/after restart
        uint32_t max = type == 0 ? UINT8_MAX : type == 1 ? UINT16_MAX : UINT32_MAX;
        const uint32_t full[] = {0,1,2,max};
        const uint32_t full_expected[] = {max,0,1,max,2,0};
        check(8, full, 4, type, false, max, full_expected, 6); // all-ones is a vertex if restart is off
        check(8, full, 4, type, true, max, NULL, 0);
        const uint32_t quads[] = {0,1,2,0,2,3};
        const uint32_t fan[] = {0,1,2,0,2,3,0,3,4};
        check(7, vertices, 7, type, false, 0, quads, 6);
        check(6, vertices, 5, type, false, 0, fan, 9);
        check(9, vertices, 5, type, false, 0, fan, 9);
    }
    uint32_t n;
    uint32_t *indices = expand_indices(8, 7, NULL, 0, 100, true, 103, &n);
    assert(n == 12); // draw-arrays ignores index restart, and preserves first vertex IDs
    for (unsigned i = 0; i < n; ++i) assert(indices[i] == strip[i] + 100);
    free(indices);
    // Long strips retain pair continuity across 65536 quads (the upstream batch size).
    indices = expand_indices(8, 131078, NULL, 0, 10, false, 0, &n);
    assert(n == 393228 && indices[n - 6] == 131087 && indices[n - 5] == 131084);
    free(indices);
    if (!setjmp(failed)) { expand_indices(8, UINT32_MAX, NULL, 0, 0, false, 0, &n); abort(); }
    if (!setjmp(failed)) { expand_indices(8, 4, vertices, 3, 0, false, 0, &n); abort(); }
    puts("PASS: quad-strip topology, flat vertex, pair continuity, restart/tails, unaligned indices, old topologies and bounds");
}
''')
binary = out / 'test-primitive-indices'
subprocess.run([os.environ.get('CC', 'clang'), '-O1', '-g', '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=15)
