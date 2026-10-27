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

## On-device flicker capture

Two independent buttons appear at the top of the game: **Continuous capture** (press again to stop) and
**Capture current frame**. Settings → Video also provides both buttons and the threshold. F8 toggles monitoring;
F10 captures once; F9 retains the existing attachment diagnostics. Capture defaults to off.

Following the user's reduced-data requirement, continuous mode saves **two** consecutive complete GPU frames:
before a change and the frame that crosses the threshold. The latter is the changed frame, not a recovery frame.
Single capture saves the first complete game frame after the button press, regardless of the threshold.
Each frame is a standard RenderDoc `.rdc` with commands, shaders, textures, buffers and initial resource state.
PNG files are previews. Candidates stay in anonymous `memfd` files until selected; normal frames are replaced in memory.
The threshold defaults to 8% (range 2%–40%). Game UI is included; touch controls, settings and FPS overlays are excluded.

Files remain under `/storage/emulated/0/Android/data/com.totkrecomp.android/files/flicker/<timestamp>/`:

- Continuous: `event-0001/01-before.rdc`, `02-after.rdc`, with matching PNG files.
- Single: `event-0001/frame.rdc`, `frame.png`.
- `preview.png` shows the selected one or two frames. `event.json` includes actual frame numbers, timestamps,
  capture sizes, threshold and `threshold/manual` reason.
- `status.txt` and `events.tsv` contain status, counts, dropped samples and the event index.
- Incomplete writes use `.partial` directories; completed events are renamed only after every required file is written.

Review previews on the phone first and retrieve only selected events. No automatic transfer or full-session video.
Complete GPU capture synchronises work and snapshots resources, which substantially slows the game and can alter
intermittent timing. Sampling pauses while the writer owns the two slots, then starts a fresh consecutive pair.
Retained serialised RDC data is limited to 2 GiB, with additional transient RenderDoc resource memory.
A session stops after reaching 64 saved events or 8 GiB.

Build the capture library with `bash android/deps/build_renderdoc.sh` inside `dev`, then build the APK.
RenderDoc 1.45 is pinned; the memory capture/custom driver replay patch and license are in `android/deps/patches/`.
A missing library reports capture unavailable. `LARECOMP_DISABLE_GPU_CAPTURE=1` disables the layer at startup for comparisons.
RenderDoc 1.45 or later can inspect these files; replay additionally requires compatible GPU features and memory layouts.
A Turnip capture is not guaranteed to replay on desktop AMD or software Vulkan. The provided replay checker supports
selecting the original Turnip on the phone.

`tools/test_gpu_capture.c` exercises production capture code on a real GPU: stable-frame rejection, the selected pair,
single capture, stop, discard and restart. `tools/test_gpu_capture_replay.cpp` replays and checks preview texture pixels.
On Android set `renderdoc__replay__marker=1`, `REPLAY_DRIVER` and `REPLAY_HOOKS` before launch. Do not link the Android
checker with `-rdynamic`, which exports adrenotools private loader pointers as system function names.
The desktop checker needs `-rdynamic` to export its replay marker. These checks do not restart the running game.

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
clang -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Iandroid/deps/out/include tools/test_shader_depth_precision.c -Wl,--gc-sections \
  -lshaderc_shared -lpthread -lvulkan -o .work/totk-port/shader-depth-tests/test-depth
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

The device dialog check launches the Activity, triggers a controlled JNI runtime error, and clicks Copy and OK.
It verifies the clipboard, dialog, Activity and retained worker; Android ends the instrumentation process afterward.
This does not test recovery from native signal crashes.

```bash
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers=4 assembleDebug assembleDebugAndroidTest
adb -s "$ANDROID_SERIAL" install --streaming -r app/build/outputs/apk/debug/app-debug.apk
adb -s "$ANDROID_SERIAL" install --streaming -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb -s "$ANDROID_SERIAL" shell am instrument -w \
  com.totkrecomp.android.test/com.botwrecompandroid.RuntimeErrorDialogCheck </dev/null
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
