# Testing and diagnostics

[简体中文](../TESTING.md) | English

From the project root, enter `distrobox enter dev` and run `source android/dev_env.sh`.
Generators, compilers, Python and Java use temporary storage under `.work/totk-port/`.

## Common checks

```bash
"$PYTHON" tools/test_totk.py
"$PYTHON" tools/test_romfs_build.py
"$PYTHON" tools/test_exclusive_pair.py
"$PYTHON" tools/test_local_branches.py
"$PYTHON" tools/test_shader_control_flow.py
```

These cover input contracts/streaming extraction, RomFS building, paired-exclusive instructions, local branches and shader control flow.
Shader checks also need `cc`, `glslangValidator` and `spirv-val` in `dev`.
Other synthetic `tools/test_*.c` cases cover services, LDC, gather, packaged textures and partial texture updates;
prior results are recorded in the [investigation history](../TOTK-PORT.md).
BOTW version cases in the inherited RomFS suite do not establish TOTK resource completeness.

Run checks relevant to the change. APK commands are in [building](BUILD.md).
The inherited `tools/test_lift.py` depends on macOS/ARM64 and was not run successfully on this x86_64 Linux development host.

## Device evidence

Record the commit/APK, driver and version, environment, scene, user feedback, valid frame range and logs for each run.
Always select the correct OnePlus ADB target. The log tag is `TOTKRecompAndroid`.
Upstream device results are not this branch's acceptance results. Short normal sequences do not disprove intermittent user reports.
Exclude stale files left by previous runs.

`LARECOMP_NO_SCALE=1` disables runtime scaling only. After the upstream merge, `LARECOMP_GAME_DYNRES=1` restores the game's own dynamic-resolution behavior.
The default GPU counter policy changed; record both that setting and actual attachment dimensions in comparisons.

## Captures and independent replay

`LARECOMP_DEBUG_FRAME` and `LARECOMP_DEBUG_FRAMES` select the frame range. F9 can trigger another range in the same process:

```bash
adb -s "$ANDROID_SERIAL" shell input keyevent KEYCODE_F9
```

| Variable | Purpose |
| --- | --- |
| `LARECOMP_DEBUG_NO_PASSES=1` | Disable the original per-pass image waits while retaining draw logs |
| `LARECOMP_CAPTURE_ATTACHMENTS=1` | Copy actual MRT, depth and stencil subresources at target transitions, then read back at frame end |
| `LARECOMP_DUMP_VERTEX_INPUT/OUTPUT` | Capture a selected vertex program or target's inputs and GPU outputs |
| `LARECOMP_DUMP_FRAGMENT_INPUT` | Capture supported inputs of a selected compositor fragment program |
| `LARECOMP_DUMP_TARGET` | Read color/depth for a selected guest texture at pass end |
| `LARECOMP_DUMP_GLOBAL_INPUT=1` | Capture global buffers/address tables alongside vertex diagnosis |
| `LARECOMP_SAVE_MSL=1` | Save translated shader source; it is GLSL on Vulkan |

Attachment captures use `build/capture/<frame>/manifest.tsv` to record format, dimensions, address, mip/layer and written byte count.
They are bounded to 512 MiB and 1,024 images per frame; omissions are reported. 3D, MSAA and compressed attachments are outside this diagnostic's scope.
The last group is the presented image under the same frame number as its draws.

Readback changes timing. Deferred capture reduces per-pass CPU waits but is not suitable for performance measurement.
Shader instrumentation can also alter driver optimization. A successful independent replay verifies those inputs, not every live binding or synchronization dependency.
Keep private GPU inputs, generated shader sources and replay data out of Git.
