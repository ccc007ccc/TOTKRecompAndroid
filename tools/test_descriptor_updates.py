#!/usr/bin/env python3
"""Compile the actual stage_set and helper module with synthetic resource/driver inputs."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / '.work/descriptor-update-tests').resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('typedef struct { uint8_t regular[MAX_BIND]')
(out / 'descriptor_plan.inc').write_text(source[start:source.index('\n#endif', start)])
start = source.index('#if defined(__ANDROID__) && TOTK_DIAGNOSTICS\nstatic bool descriptor_updates_enabled(')
end = source.index('// ================================================================== compute', start)
(out / 'stage_set.inc').write_text(source[start:end])
subprocess.run(['clang', '-O1', '-g', '-D__ANDROID__', '-fsanitize=address,undefined',
                '-I'+str(root/'android/deps/out/include'), '-I'+str(root/'android/deps/out/volk'), '-I'+str(out),
                str(root/'tools/test_descriptor_updates.c'), str(root/'runtime/nvn/vulkan/descriptor_update.c'),
                str(root/'android/deps/out/volk/volk.c'), '-Wl,--wrap=calloc', '-ldl', '-o', str(out/'stage-set')], check=True)
for mode in ('template', 'legacy', 'failure', 'allocation', 'no-cache', 'plan'):
    subprocess.run([str(out/'stage-set'), mode], check=True, timeout=15)
