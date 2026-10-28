# Phone performance sessions

Use `benchmark` for routine measurements: automatically load an isolated save, measure continuously with a **60 FPS target**, then close the game. The default measurement lasts 15 seconds after loading. It requires no pause, screenshot or step-by-step confirmation. The older paused, fixed-frame-count session remains available for explicit diagnostics.

Direct loading is verified on OnePlus 13 / Adreno 830: TotK 1.4.3 slot_00 loads the sky-island cliff without button input. Historical cached startup times were about 45–49 seconds for O0, 30–38 seconds for O1 and 31 seconds for full O3/8 Elite. These are loading times, not FPS gains.

## Isolated test package

Inside `dev`, source the main checkout's `android/dev_env.sh`, enter this branch's `android/botwrecompandroid`, and run:

```sh
bash gradlew --offline -PperformanceTest=true assembleDebug
```

This builds `com.totkrecomp.android.perftest` (TOTK Performance Test), which coexists with the ordinary app. Omit the property for an ordinary build. Import or copy the player's own game data once into the test package, matching drivers and settings. Install, launch and stop only this package during tests so another task updating the ordinary app cannot terminate the session. Always select the intended ADB device.

Run inside `dev`, after sourcing the main checkout's `android/dev_env.sh`:

```sh
"$PYTHON" tools/phone_test_session.py --serial 192.168.0.12:45117 \
  --package com.totkrecomp.android.perftest \
  --output /home/deck/Code/Games/TOTKRecompAndroid/.work/totk-port/performance-phone \
  benchmark --save-slot 0 --seconds 15
```

The default seed is the test package's `files/performance/seed`; override it with `--save-seed`. Every run creates a fresh `benchmark-*` directory and complete save copy. `--seconds` accepts 1–60 seconds. Routine measurements use only the 60 FPS target. `--game-library <absolute phone path>` selects an existing matching library without redeployment; the APK and library must share a profile and GAMELIB_ID.

The runtime starts its timer after the existing readiness heuristic: 90 consecutive frames with a scene G-buffer and sufficient draws. This is not the game's internal playable-state flag. On expiry, it atomically publishes `sample-0001.json` and final status, then requests process exit. Startup beyond 120 seconds or leaving the foreground also ends the run. If rendering stops producing frames, the event thread can still end the timer; that wait remains part of elapsed time. The controller collects results, logs, temperatures and battery state, and closes the test package after errors or failure to exit. The native timer continues if the host disconnects.

`launch.json` records APK/library hashes, environment and which side closed the process. Results include target rate, actual FPS, elapsed time and frame-time percentiles. Routine benchmarks disable per-frame diagnostic counters and GPU capture, without the manual warm-up/pause workflow. All four 30/45/60/120 game options remain available; benchmarks use 60.

## Explicit interactive diagnostics

Use the older `start` mode when retaining a scene, taking manual samples or investigating rendering:

```sh
PHONE_TEST=("$PYTHON" tools/phone_test_session.py --serial 192.168.0.12:45117 \
  --package com.totkrecomp.android.perftest \
  --session /data/user/0/com.totkrecomp.android.perftest/files/performance/session \
  --output /home/deck/Code/Games/TOTKRecompAndroid/.work/totk-port/performance-phone/direct-slot-00)
"${PHONE_TEST[@]}" \
  start --save-seed /data/user/0/com.totkrecomp.android.perftest/files/performance/seed \
  --save-slot 0
```

Slots 0..5 mean the game's `slot_00..slot_05` directories, not the date-sorted menu rows. The runtime clones the entire save root, including store metadata, into a new private `save-*` directory before boot. Missing slots and reused destinations are rejected. The source save is unchanged.

The runtime checks the original TotK 1.4.3 AINB resources and creates private temporary overrides from the player's files. `NormalTitle.140.module` enters the existing confirmed-load branch. Four index inputs in `OnLoadGameSaveData.module` select progress, auxiliary data, footprint and caption from the requested slot. The existing load/reset/post-load branch is retained, but intermediate loading-screen states are not guaranteed to match manual entry. Modified or unsupported source resources are rejected. No game assets are bundled in the APK, and normal launches do not enable these patches.

Known limitation (2026-10-28): automatic direct loading can leave the left grid background black. The user confirms normal manual loading in the same test app and has deferred this issue. Intermittent missing grass also occurs in the ordinary app and is deferred separately; it is neither established as a test-entry fault nor fixed. A recent grassland save has been copied without modifying its source and loaded automatically. It differs from the bare-cliff seed used for historical performance results, so those scenes must not be mixed in comparisons.

This skips menu interaction while retaining the existing title-entry setup. `--auto-load` retains the older button simulation for compatibility; it is not direct loading. Use `--session` to select a separate private control directory, consistently for all commands.

The example seed is a complete save copy prepared beforehand. `start --game-library <absolute phone path>` selects an existing alternative library through `LARECOMP_GAME_LIB`, such as a private O1 library, without replacing the default one. The tool verifies app readability and records APK and explicitly selected library SHA256 hashes in `launch.json`.

```sh
"${PHONE_TEST[@]}" status
"${PHONE_TEST[@]}" shot
"${PHONE_TEST[@]}" sample --frames 180 --cooldown 30
"${PHONE_TEST[@]}" resume
```

The renderer pauses after completing a frame and draining GPU work. The SDL event thread keeps handling controls, including the on-screen Pause test / Resume game buttons. Samples exclude 30 warm-up frames and pause again after the requested number of measured frames. `gpu` and `shot` diagnostics run separately from FPS samples.

This explicit diagnostic mode pauses on readiness and can capture the game's own window when investigating rendering. A 120-second startup timeout reports an error, and status queries or screenshots cannot make that session eligible for FPS sampling. Routine `benchmark` runs do not use this pause/screenshot workflow.

Results include mean, median, p95 and FPS for a fixed frame count, logs, and before/after thermal and battery reports. Use `sample-*.json`; ordinary FPS logs and overlays can include the long pause immediately after resuming.

`shot` reads only the game's own window. It writes `window.png` in the private control directory and retrieves it using `run-as tar` as `view-<frame>.png`, avoiding external build-directory permission issues. Use **Current temperatures from HAL** in thermalservice reports; the earlier cached notifications can contain stale high temperatures.

The pre-capture baseline is `d9c6d52` plus the same `43716c5` black-patch fix and session implementation. Match the save slot, game library, driver, graphics settings, viewpoint, warm-up and temperature. Do not compare different scenes or simpleperf-instrumented samples against uninstrumented samples.

The 2026-10-28 comparison used the same save, Banner Turnip `v27.0.0-20261025-r5-A8xx`, 8-bit gather and full O0 game library. Diagnostic counters were enabled, actual capture and RenderDoc disabled, and no simpleperf ran during FPS samples. Each version received 240 measured warm-up frames, followed by 180-frame samples with 30 excluded warm-up frames each and at least 30 seconds paused for cooling:

| Version | Individual FPS | Pooled FPS |
| --- | --- | --- |
| Pre-capture + same black-patch fix | 19.468, 19.471 | 19.469 |
| Current capture version, before getenv optimization | 19.704, 18.567, 19.896 | 19.371 |

Battery temperatures were 29.3–29.5°C for the baseline and 28.7–29.5°C for current; thermal status was always 0. Pooled FPS is total frames divided by total time, retaining the slower third sample. The roughly -0.5% difference does not reproduce a large regression in this view. Short samples of one viewpoint do not rule out losses in other scenes or while actively capturing.

Existing measurements suggest significant resource binding and constant-copy costs (about 6.9 ms/frame in one run56 interval). About 1.4 GiB/two seconds was constant-buffer copying, not texture upload. The latest separate render-thread CPU profile attributed 14.41% to `memmove`, 13.81% to `upload_texture` and 11.71% to `getenv`; these are thread CPU-cycle sample shares, not predicted FPS gains. Use `simpleperf record --in-app` for collection.

This branch makes `vertex_debug_capturing()` return early for ordinary frames, avoiding environment queries on each vertex constant binding while preserving debug-frame behavior. A separate profile after the change collected 1139 render-thread samples: `getenv` fell to 0.10%, with `memmove` at 15.56% and `upload_texture` at 15.70%. Three samples without simpleperf measured 18.302, 19.332 and 19.693 FPS, without a demonstrated consistent FPS gain. Changes in sample shares across runs are not equivalent changes in total CPU time.

The old library used `TOTK_FAST_COMPILE=ON`, making every generated function O0 for first-boot debugging. The historical O1 baseline below disabled that switch while retaining O0 for huge units in heavy.txt. Current `performance` and `snapdragon-8elite` profiles use O3 + ThinLTO for every unit and reject O0 exceptions; see [Build](BUILD.md). NDK r30 exhibits pathological MemorySSA updates during LICM memory promotion; CMake defaults `TOTK_DISABLE_LICM_PROMOTION=ON` to disable just that step. Set it OFF to reproduce the original compiler behavior. The O1 library passed the short device checks below; they do not establish correctness throughout the game.

The generated 1.4.3 `main/main_516.c` contains one function with 11937 labels. Despite falling below the old 15000-label threshold, its SimpleLoopUnswitch MemorySSA updates exhaust memory. This build explicitly adds it with `-DTOTK_EXTRA_HEAVY_SOURCES=main/main_516.c`. The option defaults to empty and leaves the shared generated/heavy.txt untouched. The same failure was subsequently confirmed in main/main_484.c (10069 labels) and main/main_568.c (9746 labels), and all three were explicitly listed as exceptions. The final O0 unit list is read from the CMake cache into the build record; other units remain at O1. The final build has 16 O0 units. Reduce concurrency on memory-constrained hosts; compilation completed at `-j2` and linking ran as a separate single job. Record these exceptions and compiler settings with performance results.

## O0 / O1 device results and the 20 FPS mode

The same APK (SHA256 prefix `a8e5a347d6f7d0f4`), save, view, driver and settings were used at 27.3–27.5°C battery temperature. Each library received 240 warm-up frames followed by three 180-frame samples, each excluding 30 additional warm-up frames and cooling paused for at least 30 seconds:

| Game library | Individual FPS | Pooled FPS | Individual p95 frame times |
| --- | --- | --- | --- |
| Original full O0 | 19.929, 18.198, 19.326 | 19.124 | 60.19, 67.44, 63.68 ms |
| O1 with 16 huge units at O0 | 20.000, 20.000, 19.999 | 20.000 | 51.27, 51.20, 51.15 ms |

Average FPS improved by about 4.6% in this view, with substantially fewer long frames. The deployed library shrank from 525059280 to 276045768 bytes. O0 SHA256 starts with `c5265ce36cbecb55`; O1 starts with `507fe278e9dc73de`. The retained O1 symbols have Build ID `844486e05d5e9e10c004b580e7aa635d7d8f3ea5`.

Those historical samples have an explicit pacing limit near 20 FPS: the log shows the game selecting present interval 3, and `nvnQueuePresentTexture` waits three 60 Hz periods. At the time, variable-clock integration in `framepace_on()` supported only BOTW 1.8.2 and was inactive for TotK 1.4.3. This branch subsequently added TotK 30/45/60/120 targets and measured time steps; see [Upstream integration](UPSTREAM-INTEGRATION.md) for the mechanism and validation limits. A separate old O1 CPU profile attributed 51.55% of process cycle samples to the game library. Render-thread hotspots remained `memmove` (16.45%), `upload_texture` (15.36%), binding and drawing. These shares are not direct measurements of critical-path time.

`start --diagnostic-present-interval 2` retains the older interval diagnostic, enabling `LARECOMP_FORCE_INTERVAL=2` and pacing logs. Samples are marked `present-interval-diagnostic`; the override can alter game speed and is not a validated gameplay improvement or default. That historical run was removed from the task list before sampling, so it produced no valid FPS result. Current frame targets use the later TotK measured-clock integration described in [Upstream integration](UPSTREAM-INTEGRATION.md).

## O1 and full O3 measurements after frame-rate integration

Existing 2026-10-28 results use the same slot_00 cliff, the 60 FPS target and three 180-frame samples:

| Build | Individual FPS | Conditions |
| --- | --- | --- |
| New timing, O1 with two huge O0 units | 27.936, 25.973, 26.069 | Battery about 30°C, not charging |
| Full O3/ThinLTO, 8 Elite | 22.216, 23.536, 22.873 | Battery about 35.6°C, charging |

Different temperature and charging conditions prevent a conclusion about O3 improvement or regression. The user requested no repeat of this build comparison; the original measurements and their limitations are retained. O1 short checks at 30/45/120 targets measured about 27.78/27.61/27.46 FPS. These establish execution of target/timing paths under limited throughput, not stable performance above 30 FPS or 120 FPS gameplay.

The full O3 library came from `aaf348b`: all 1755 generated units use `-O3 -flto=thin -fno-fast-math`, with no O0 fallback. The 8 Elite flags are `-march=armv8.7-a+crypto+fp16 -mtune=oryon-1`. The 264946296-byte library has SHA256 `5f3156b208076afa8b07d831d538977e643083ed6014bfd8711760d5137f087c` and GAMELIB_ID `993c0a5c82dc5099`; that APK's SHA256 is `ac037f1031d1e42a171182d4b434784d3750351f6f5fbda6f34973b9ffb9c457`. Later runtime-only fixes can reuse the matching library without rebuilding all game code.

Offline checks, run in `dev` with fresh output directories:

```sh
clang -O1 -g -fsanitize=address,undefined -Wall -Wextra \
  tools/test_test_boot.c runtime/test_boot.c -o "$TEST_OUT/test-boot"
"$TEST_OUT/test-boot" "$SEQUENCES/NormalTitle.140.module.ainb" \
  "$SEQUENCES/OnLoadGameSaveData.module.ainb"
clang -O1 -g -fsanitize=address,undefined -Wall -Wextra \
  tools/test_test_session.c runtime/test_session.c -lpthread -lm -o "$TEST_OUT/test-session"
"$TEST_OUT/test-session" "$TEST_OUT/new-session"
"$TEST_OUT/test-session" "$TEST_OUT/new-timeout-session" --startup-timeout
for mode in timed timed-stall timed-timeout timed-background; do
  "$TEST_OUT/test-session" "$TEST_OUT/new-$mode" "--$mode"
done
"$PYTHON" tools/test_phone_benchmark.py
```

Timed checks cover continuous sampling after readiness, no pause, stalled rendering, startup timeout and background exit. The controller tests use mock ADB to cover one launch, results, legacy APK rejection and shutdown fallback without contacting a phone. These C/controller checks do not establish full gameplay correctness on a device.

`TEST_OUT` is a project-local scratch directory and `SEQUENCES` holds original files extracted from the player's RomFS. All six slots were also checked using the independent [AINB parser](https://github.com/dt-12345/AINB), comparing complete parsed graphs and allowing only the intended entry and index changes. Do not commit game assets or generated test output.
