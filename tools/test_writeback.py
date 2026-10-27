#!/usr/bin/env python3
"""Run production write-back queue code against a delayed GPU/VMA model.

Checks saturation, wraparound, alias ordering, pitched rows and timeouts. No real GPU is used.
Run in dev: python3 tools/test_writeback.py
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.work/totk-port/writeback-tests'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
block = source[source.index('// ---- write-backs:'):source.index('TexBackend *g_dump_sampled;')]
(out / 'writeback-under-test.inc').write_text(block)
subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I', str(out), '-I', str(root / 'android/deps/out/include'),
                str(root / 'tools/test_writeback.c'), str(root / 'runtime/nvn/tegra.c'),
                str(root / 'runtime/nvn/formats.c'), '-pthread', '-lm', '-o', str(out / 'check')], check=True)
subprocess.run([str(out / 'check')], check=True)
