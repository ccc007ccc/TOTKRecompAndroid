#!/usr/bin/env python3
"""Check the actual packaged native diagnostics policy (run in dev).

Usage: check_android_diagnostics.py <apk> --expect off|on
The regular APK/ELF identity reader remains the source of profile and ABI checks.
"""
import argparse
import json
import os
import subprocess
import tempfile
import zipfile
from pathlib import Path
from package_android import apk_application_id, runtime_identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--expect', choices=['off', 'on'], required=True)
    args = parser.parse_args()
    enabled = args.expect == 'on'
    package = apk_application_id(args.apk)
    if (package != 'com.totkrecomp.android') != enabled:
        raise ValueError('Diagnostic policy does not match the application package')
    with zipfile.ZipFile(args.apk) as archive:
        names = archive.namelist()
        data = archive.read('lib/arm64-v8a/libmain.so')
    identity, profile = runtime_identity(data)
    if not enabled and any('renderdoc' in n.lower() or 'libVkLayer_' in n for n in names):
        raise ValueError('Production APK contains a GPU capture/validation layer')
    ndk = Path(os.environ['ANDROID_NDK']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    with tempfile.TemporaryDirectory() as folder:
        library = Path(folder) / 'libmain.so'
        library.write_bytes(data)
        symbols = subprocess.check_output([str(ndk / 'llvm-nm'), '-D', '--defined-only', str(library)], text=True)
    exported = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    diagnostic = {'flicker_start', 'flicker_single', 'vk_flicker_frame', 'vk_diag_create', 'rd_layer_load',
                  'test_session_enabled', 'test_boot_patch', 'vk_timing_trace_on'}
    if (not enabled and diagnostic & exported) or (enabled and not diagnostic <= exported):
        raise ValueError(f'Unexpected diagnostic backend exports: {sorted(diagnostic & exported)}')
    required = {'g_fnprof', 'fnprof_hit', 'kernel_preempt_point', 'renderer_set_scene_scale'}
    if not required <= exported:
        raise ValueError(f'Missing normal runtime interfaces: {sorted(required - exported)}')
    switches = ['LARECOMP_TEST_SESSION', 'LARECOMP_FULL_GPU_CAPTURE', 'LARECOMP_FNPROF',
                'LARECOMP_DUMP_VERTEX_OUTPUT', 'LARECOMP_AUDIO_DUMP', 'LARECOMP_TIMING_TRACE',
                'LARECOMP_AUDIO_WAV', 'LARECOMP_AUDOUT_WAV', 'LARECOMP_VK_VALIDATION', 'LARECOMP_LAUNCHER_DELETE',
                'LARECOMP_VK_WORKER', 'LARECOMP_VK_DESCRIPTOR_UPDATES',
                'LARECOMP_VK_LOADOP_CLEAR', 'LARECOMP_LOG_PASS_CLEARS']
    for name in switches:
        present = name.encode() + b'\0' in data
        if present != enabled:
            raise ValueError(f'{name}: unexpected compiled diagnostic switch')
    if any(n.startswith(('cw_vk', 'vk_command_worker_', 'vk_descriptor_update_')) for n in exported):
        raise ValueError('Internal Vulkan experiment interfaces must not be exported by libmain')
    print(json.dumps(dict(package=package, profile=profile, gamelib_id=identity, diagnostics=enabled,
                          diagnostic_exports=sorted(diagnostic & exported)), indent=2))


if __name__ == '__main__':
    main()
