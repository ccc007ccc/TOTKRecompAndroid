#!/usr/bin/env python3
"""ASan/UBSan checks of production submit snapshot capture and replay, without a GPU."""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/submit-snapshot-tests"
OUT.mkdir(parents=True, exist_ok=True)
binary = OUT / "test-submit-snapshot"
subprocess.run(["clang", "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-D__ANDROID__",
                "-DSUBMIT_SNAPSHOT_BYTES=4096", "-DSUBMIT_SNAPSHOT_BUDGET=8192", "-DSUBMIT_SNAPSHOT_RANGES=32",
                "-DSUBMIT_SNAPSHOT_WRITES=8", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                str(ROOT / "tools/test_submit_snapshot.c"), str(ROOT / "runtime/nvn/submit_snapshot.c"),
                "-lpthread", "-o", str(binary)], check=True)
for enabled in ("1", "0"):
    env = dict(os.environ, LARECOMP_VK_SUBMIT_SNAPSHOT=enabled)
    subprocess.run([str(binary), *(["--disabled"] if enabled == "0" else [])], check=True, env=env)

# Exercise the production pool registry separately, including aliases and remapping, rather than relying solely on
# the capture test's bounded reader double. No guest object initialization or Vulkan calls are needed here.
objects = (ROOT / "runtime/nvn/objects.c").read_text()
start = objects.index("typedef struct SubmitPool")
registry = objects[start:objects.index("\n#endif", start)]
driver = OUT / "pool-registry.c"
driver.write_text('''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/nvn.h"
#undef RT_ON_RESET
#define RT_ON_RESET(name) static void name(void)
''' + registry + '''
int main(void) {
    unsigned char bytes[256], out[64];
    memset(bytes, 17, sizeof bytes);
    uint64_t base = (uint64_t)(uintptr_t)bytes, lo, hi;
    NvnPool first = {.storage = base, .size = 64};
    NvnPool second = {.storage = base + 64, .size = 64};
    submit_pool_update(&first); submit_pool_update(&second);
    assert(nvn_submit_read(base + 16, 16, out, &lo, &hi));
    assert(lo == base && hi == base + 64 && out[0] == 17);
    assert(!nvn_submit_read(base + 32, 64, out, &lo, &hi));
    assert(!nvn_submit_read(UINT64_MAX - 8, 16, out, &lo, &hi));
    assert(!nvn_submit_read(base, 0, out, &lo, &hi));
    NvnPool alias = {.storage = base + 32, .size = 64};
    submit_pool_update(&alias);
    assert(nvn_submit_read(base + 32, 64, out, &lo, &hi));
    submit_pool_remove(&alias);
    assert(!nvn_submit_read(base + 32, 64, out, &lo, &hi));
    first.storage = base + 128;
    submit_pool_update(&first);
    assert(!nvn_submit_read(base, 16, out, &lo, &hi));
    assert(nvn_submit_read(base + 128, 16, out, &lo, &hi));
    submit_pool_remove(&first); submit_pool_remove(&second);
    assert(!nvn_submit_read(base + 128, 16, out, &lo, &hi));
    submit_pool_update(&first); submit_pools_reset();
    assert(!nvn_submit_read(base + 128, 16, out, &lo, &hi));
    puts("Production pool registry: bounds, aliases, remapping, finalization and reset passed");
    return 0;
}
''')
pool_binary = OUT / "test-pool-registry"
subprocess.run(["clang", "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-D__ANDROID__",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "runtime"),
                str(driver), "-lpthread", "-o", str(pool_binary)], check=True)
subprocess.run([str(pool_binary)], check=True)
