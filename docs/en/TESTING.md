# Testing and diagnostics

Capture, scripted input, Intent `env` overrides and built-in probes require Debug or the independent performance-test package.
Ordinary Release disables native `TOTK_DIAGNOSTICS`; verify actual APKs with `tools/check_android_diagnostics.py <apk> --expect off|on`.
The Release regression `tools/test_release_diagnostics.c` checks that diagnostic environment variables stay inert while scheduler preemption still works.

[简体中文](../TESTING.md) | English

From the project root, enter `distrobox enter dev` and run `source android/dev_env.sh`.
Generators, compilers, Python and Java use temporary storage under `.work/totk-port/`.

## Common checks

```bash
"$PYTHON" tools/test_totk.py
"$PYTHON" tools/test_romfs_build.py
"$PYTHON" tools/test_exclusive_pair.py
"$PYTHON" tools/test_local_branches.py
"$PYTHON" tools/test_totk_framepace.py
bash tools/test_present_interval.sh
"$PYTHON" tools/test_shader_control_flow.py
"$PYTHON" tools/test_shader_driver_constants.py
```

These cover input contracts/streaming extraction, RomFS building, paired-exclusive instructions, local branches and shader control flow.
Frame-timing checks execute generated controller interval/FPS stores and fade conversions, cutscene expiry/reset,
and NVN window state. Add `test_totk_framepace.py --image <private main-image.bin>` for real instruction guards;
see the [2026-10-30 record](../FRAMEPACE-20261030.md) for evidence and gameplay limits.
Shader checks also need `cc`, `glslangValidator` and `spirv-val` in `dev`.
Other synthetic `tools/test_*.c` cases cover services, LDC, gather, packaged textures and partial texture updates;
prior results are recorded in the [investigation history](../TOTK-PORT.md).
BOTW version cases in the inherited RomFS suite do not establish TOTK resource completeness.

Run checks relevant to the change. APK commands are in [building](BUILD.md).
The inherited `tools/test_lift.py` depends on macOS/ARM64 and was not run successfully on this x86_64 Linux development host.

For the automatic phone benchmark, `--gpu-profile-frame 1150` timestamps one absolute frame and
`--timing-trace` records queue waits and GPU counter pairs. Both enable profiling; timestamp readback changes
timing, so keep these results separate from ordinary FPS measurements. `--render-split-us 0..20000` overrides
the long-pass submission interval without enabling profiling; 0 disables mid-pass splitting. All three options
are recorded in `launch.json`. See the [phone workflow](../PHONE-PERFORMANCE.md) for the full command and device rules.

Shared-list parsing, deduplication, game/stage filtering and type preservation, plus cache-first runtime loading:

```bash
bash tools/test_shader_warm_list.sh
bash tools/test_shader_warm_prepare.sh .work/shader-warm-check .work/totk-port/game/romfs.bin
bash tools/test_shader_warm_background.sh .work/shader-background-check .work/totk-port/game/romfs.bin
bash tools/test_shader_runtime.sh
```

The first command optionally takes an output directory and a local RomFS image to check the complete production inventory.
The second compiles real title requests, disables compiler access, strictly reloads the cache, and checks numeric interfaces and linked varying masks.
The background test covers lifecycle, recency, cancellation/profile changes and cache retention against real requests.
See the [balanced preparation work](../SHADER-BALANCED.md). These checks do not establish full Android game startup or frame-time performance.

## Device evidence

See [phone performance sessions](PHONE-PERFORMANCE.md) for direct save selection, automatic timed 60-target measurements
and exit, explicit interactive diagnostics, and the current device-validation status and limitations.

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

## On-device flicker capture

The game keeps two independent buttons: **Continuous capture** (press again to stop) and **Capture current frame**.
F8 toggles monitoring, F10 captures once, and Settings → Video adjusts the threshold.
The default is now **lightweight diagnostics**, without loading RenderDoc or copying entire GPU allocations.
Continuous mode compares on-device images and saves the consecutive before/changed pair only above the threshold;
single capture ignores the threshold. The changed frame is not a recovery frame. Game UI is included; application overlays are excluded.

Each frame includes:

- Color, depth and supported stencil thumbnails at render-target transitions, with addresses, formats, mip/layer and group indices.
  Their longest side is 128 pixels. Supported normalized/float color uses RGBA16F to preserve HDR values; depth retains its native format.
  Integer color and other unsupported formats retain metadata and an omission reason without issuing illegal format conversions.
  These thumbnails do not replace full-resolution pixel/input validation.
- Draw, skipped draw, clear and compute records; viewport/scissor, depth/stencil state, texture/sampler and vertex bindings.
- Shader machine code and translation parameters, plus bounded uniform **CPU mirrors**. GPU-written inputs can differ until writeback;
  use targeted capture when exact inputs are needed.
- A final-game PNG up to 960 pixels wide and `preview.png` for the selected frame(s).

Per-frame budgets are 24 MiB for stage images, 8 MiB for code/inputs and 4 MiB for logs. The two-frame core buffers use about 80 MiB,
with additional driver/encoder overhead. Shader records are capped at 128 KiB each and constants at 4 KiB, with original and saved lengths.
Log space is reserved for later draw/skip summaries and shader code/key associations. Exceeding a data budget records omissions instead of discarding the event.

For targeted inputs, set `LARECOMP_DIAGNOSTIC_VS=<hex vertex-code address>` or `rt:<width>x<height>:<hex NVN format>`.
The first matching draw in each captured frame records sampled GPU thumbnails and CPU storage mirrors (up to 2 MiB each)
within the existing budgets. `sampled` log rows connect bindings to `image_slot=16+binding` and `cpu_blob`.
Images show the selected mip/layer backing before view swizzles; overlapping textures and sampler borders are also logged.
This adds GPU synchronization and is not a performance measurement. `LARECOMP_TRACE_GPU_COPIES=1` logs up to 256 GPU texture
source overlaps in buffer copies. `LARECOMP_WATCH_TEXADDR` also logs explicit CPU texture flushes covering that address.
Files use gzip; actual size depends on scene contents. The threshold defaults to 8% (2%–40%). A light session stops after 64 events or 512 MiB.

Files remain under `/storage/emulated/0/Android/data/com.totkrecomp.android/files/flicker/<timestamp>/`:

- Continuous events have `01-before/`, `02-after/` and matching PNGs; single capture has `frame/` and `frame.png`.
- Each frame directory contains `diagnostic.json`, `stages.bin.gz`, `inputs.bin.gz` and `draws.txt.gz`.
- The index reports omitted data; image status is `ok/unsupported/budget/allocation`. Input ID 0 means not saved.
- `event.json` uses `light-diagnostic-v1` and includes frame numbers, timestamps, stored sizes and `threshold/manual` reason.
- The session has `status.txt` and `events.tsv`. Incomplete writes use `.partial`, renamed only when complete.

Selection stays on the phone; normal frames do not reach storage. Review previews first and retrieve only selected events.
Light capture still adds GPU copies and one completion wait per frame, so it can affect intermittent timing and is not a performance benchmark.
Sampling pauses during writes and resumes with a fresh consecutive pair. Stop/interruption drains diagnostic GPU work before releasing buffers.

Rank changes between matched stage images and generate only a few comparisons:

```bash
"$PYTHON" tools/analyze_light_capture.py <event-directory> --preview .work/totk-port/stage-changes.png
```

For reproducible bugs, the existing F9 attachment diagnostics, `LARECOMP_DUMP_TARGET` (full-resolution color/depth/stencil),
or `LARECOMP_SKIP_FSHASH`/`LARECOMP_SKIP_CS` comparisons can narrow the relevant stage without a full GPU snapshot.

Full replay is an advanced startup option: `LARECOMP_FULL_GPU_CAPTURE=1`. Build the optional layer using
`bash android/deps/build_renderdoc.sh` before packaging. This mode still writes `.rdc`, can consume several GiB and is clearly labelled in the UI.
If the library is absent, the buttons use light diagnostics. `LARECOMP_DISABLE_GPU_CAPTURE=1` prevents layer loading.
RDC replay needs compatible GPU features/memory layout; the same-phone/Turnip replay checker and patches remain available.

After building the APK, run the real-GPU regression inside `dev` with the intended device and installed driver path:

```bash
ANDROID_SERIAL=<phone-serial> REPLAY_DRIVER=<driver-path-on-phone> bash tools/test_light_capture.sh
```

It verifies half-float color/depth pixels, safe integer-format rejection, before/changed selection, single capture, stable-frame rejection, cancellation/restart and stopping with pending GPU work.
It fills all three budgets to check graceful omission, successful event saving and preservation of late draw summaries, retrieving only synthetic results.
`tools/test_gpu_capture.c` and `tools/test_gpu_capture_replay.cpp` remain for full-mode regression.

Real-game validation on OnePlus 13 / Banner Turnip r5 captured the blue spawn-platform black patch at frame 1906:
the selected single-frame event is 4,837,160 bytes (about 4.6 MiB), with 202 stage images and no image/input omissions.
Verbose bindings were partly omitted; all 1,537 draw attempt/result summaries remained. This validated capture usability;
the subsequent material-stage investigation traced the black patch to texture tiling, as described below.

Image difference regression:

```bash
cc -O1 -g -fsanitize=address,undefined tools/test_flicker_detect.c -lm \
  -o .work/totk-port/scratch/test-flicker
.work/totk-port/scratch/test-flicker
```

The TMML regression executes the production emitter's output for bound/bindless textures, result masks,
signed fractional fixed-point values and sampler clamps. Maxwell returns computed LOD followed by
accessed mip level; GLSL returns them in the opposite order:

```bash
mkdir -p .work/totk-port/shader-tmml-tests
cc -O1 -g -fsanitize=address,undefined tools/test_shader_tmml.c -lm \
  -o .work/totk-port/shader-tmml-tests/generate
.work/totk-port/shader-tmml-tests/generate .work/totk-port/shader-tmml-tests
c++ -fsanitize=undefined .work/totk-port/shader-tmml-tests/tmml.cpp \
  -o .work/totk-port/shader-tmml-tests/check
.work/totk-port/shader-tmml-tests/check
for s in .work/totk-port/shader-tmml-tests/*.frag; do
  glslangValidator --target-env vulkan1.3 -V "$s" -o "$s.spv"
  spirv-val --target-env vulkan1.3 "$s.spv"
done
```

This also generates six synthetic fragment shaders for GPU sampler validation. Passing host assertions
does not establish in-game visual acceptance.

The depth-precision regression uses the production shaderc path to check vertex and explicit fragment depth,
ordinary color fragments, caching and repeated processing:

```bash
mkdir -p .work/totk-port/shader-depth-tests
sh tools/build_host_shader_compiler.sh .work/totk-port/shader-depth-tests
export LARECOMP_NATIVE_LIB_DIR="$PWD/.work/totk-port/shader-depth-tests"
clang -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Iandroid/deps/out/include tools/test_shader_depth_precision.c -Wl,--gc-sections \
  -lzstd -ldl -lpthread -lvulkan -o .work/totk-port/shader-depth-tests/test-depth
.work/totk-port/shader-depth-tests/test-depth "$PWD/.work/totk-port/shader-depth-tests"
for s in .work/totk-port/shader-depth-tests/*.spv; do spirv-val --target-env vulkan1.3 "$s"; done
```

Also verify depth-EQUAL draws on the target GPU, with a fragment shader writing depth and a vertex shader recomputing it.
Run46 tested all 256 encoded values using the same Banner Turnip build. Before the fix, 12 D16 values failed,
including water; modules produced by the fixed compiler passed every value. These are synthetic encoded values,
not a count of missing game materials or effects.


The conditional-clear regression uses the real command recorder and checks execution-time values, one-time clearing, ignored timestamps and 64-bit comparisons. Build it with Clang:

```bash
clang -O2 -ffunction-sections -fdata-sections tools/test_render_condition.c \
  -Wl,--gc-sections -pthread -o .work/totk-port/scratch/test-render-condition
.work/totk-port/scratch/test-render-condition
```

The CPU texture test covers independent GOB bytes, mips, arrays, 3D, strides, compressed atlas tails and rejected bounds.
It also covers `0x40` linear targets in the 260×260 / 68×68 height and mask formats, explicit row strides,
default alignment, final-row updates and padding preservation:

```bash
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime \
  tools/test_texture_read.c runtime/nvn/tegra.c runtime/nvn/formats.c \
  -o .work/totk-port/scratch/test_texture_read
.work/totk-port/scratch/test_texture_read
```

The GPU upload-layout test executes production `deswizzle.c` and its built-in GLSL against the existing CPU
`tegra_deswizzle` reference. Run inside `dev` with Vulkan 1.3, Khronos validation, glslc and spirv-val:

```bash
bash tools/test_vk_deswizzle.sh .work/totk-port/tests/deswizzle
"$PYTHON" tools/test_texture_change_policy.py .work/totk-port/tests/texture-change
```

It covers BC/ASTC block shapes, mips, array layers, 3D, a large atlas, dispatch limits, rejected bounds,
partial initialization failure and reset. Mutating and freeing the source before submission verifies upload snapshots.
Synchronization validation must report no warnings or errors. The change-policy test executes production decisions
for notified/unannounced writes, cached/uncached pools, linear textures, periodic checks, restyling, diagnostic lifetime
and the disabled comparison switch. These tests do not prove phone performance or long-session visual correctness.
See the [texture investigation (Chinese)](../PERFORMANCE-TEXTURES-20261030.md) for experimental switches and device results.

The default Tegra tile-height regression uses synthetic 160x160 BC3 data at independent fixed GOB addresses.
It checks rounding boundaries, all 1,600 blocks, mip offsets, and explicit/3D layout overrides:

```bash
cc -O1 -g -fsanitize=address,undefined tools/test_tegra_tile_height.c runtime/nvn/tegra.c \
  -o .work/totk-port/scratch/test_tegra_tile_height
.work/totk-port/scratch/test_tegra_tile_height
```

The real spawn-platform texture has 40 compressed rows: 1.5 times that height is 60 rows, or seven complete GOBs.
Rounding up to eight selected an eight-GOB tile, while the data uses four. The old layout read 480 of the 1,600 blocks
as zero padding; the corrected layout recovers all blocks. A separate 82x164 BC3 texture confirms the same boundary.
Changing only this texture's layout and re-uploading it in the same running OnePlus 13 game/view removed the rectangles,
without suppressing draws or substituting material colors.

The device dialog check launches the Activity, triggers a controlled JNI runtime error, and clicks Copy and OK.
It verifies the clipboard, dialog, Activity and retained worker; Android ends the instrumentation process afterward.
This does not test recovery from native signal crashes.

```bash
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers=4 assembleDebug assembleDebugAndroidTest
adb -s "$ANDROID_SERIAL" install --streaming -r app/build/outputs/apk/debug/app-debug.apk
adb -s "$ANDROID_SERIAL" install --streaming -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb -s "$ANDROID_SERIAL" shell am instrument -w \
  com.totkrecomp.android.debug.test/com.botwrecompandroid.RuntimeErrorDialogCheck </dev/null
```

The output must include `PASS: Copy preserved the dialog`; the adb exit code alone is insufficient.

The mixer test uses synthetic wire parameters and known samples without game files or an audio device.
It covers splitters, ADPCM, auxiliary rings, limiting and 3D reverb:

```bash
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime -pthread \
  tools/test_audio_mixer.c -lm -o .work/totk-port/scratch/test_audio_mixer
.work/totk-port/scratch/test_audio_mixer
```

`LARECOMP_AUDIO_STATS=1` reports direct/split/unconnected channels and decoded samples every 200 audio frames.
Unconnected voices may be intentionally disabled by the game, so they are not automatically missing sounds.
`LARECOMP_AUDIO_DUMP=1` saves update snapshots, including the initial full configuration.
`LARECOMP_AUDIO_WAV=<path inside the app data directory>` records audren's float stereo output; this is not a measure of complete game-audio coverage.

The on-device preparation path is incomplete: all 8,697 inputs now translate in both FP32-preferred and integer-binary64 modes,
but exhaustive state-variant preparation remains impractically large and has not completed.
Its strict readiness gate prevents gameplay until every shader variant is prepared.
Android defaults to experimental native FP32 arithmetic for nearest-even DADD/DMUL/DFMA; the Prelaunch shader-precision
switch restores GPU integer binary64 emulation and invalidates preparation readiness.
Both modes pass production SPIR-V validation for the 30 formerly failing double-precision programs.
`tools/test_shader_fp32.py` covers 92 synthetic shaders and 2,600,960 normal-range conversion cases on the CPU.
These checks do not prove GPU binary64 numerical accuracy, visual correctness or performance improvements.
See [the preparation audit](../SHADER-PREPARATION.md) for scope and validation limits.

The [asset linkage audit](../SHADER-ASSET-LINKS.md) associates all 8,697 shader inputs with file-defined programs
and matches static options for 54,541 materials. `tools/test_shader_catalog.cpp` checks bounded parsing,
program stage links, option keys/defaults, vertex mappings and byte-for-byte agreement with the original inventory.
Host ASan/UBSan and the same native checks on PJZ110 pass. File-defined candidates are not a complete runtime-state set.

The production planner now uses those interfaces, conservative unknown-input choices and file-defined stage pairs:
694,772 planned entries versus 318,627,294 previously. `tools/audit_shader_variants.cpp` reproduces the plan natively.
Variant-rule and queue tests cover default input types, unknown formats, deduplication, errors and compiler-session closure.
736 real shader combinations pass SPIR-V validation, including modules loaded from the production compressed cache.
Preparation defaults to one worker per logical CPU core, with a retained compiler per worker; the draw path only decompresses and validates cached bytes.
This is not a completed full-library preparation or gameplay validation.

Closeout checks: `"$PYTHON" tools/test_shader_lifetimes.py`, `bash tools/test_shared_memory.sh`, and
`bash tools/test_buffer_copy.sh` run in `dev`. Full-receipt validation is covered by `test_shader_dictionaries.sh`.
See the [closeout record](../PROJECT-CLOSEOUT.md) for scope and device results.
