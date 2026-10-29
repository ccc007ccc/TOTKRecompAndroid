# Build and deployment

[简体中文](../BUILD.md) | English

This guide targets TOTKRecompAndroid with Switch 1.4.3 inputs. See [status](STATUS.md) for current limitations.
Run commands from the project root unless stated otherwise.
The [project conventions (Chinese)](../../Agent.md) define the shared workflow and coding rules;
the [2026-10-29 baseline](../PROJECT-BASELINE.md) reconciles the recent shader and memory work.

## Environment and APK

SteamOS is immutable. Use the existing Distrobox container named `dev` for builds, dependency installation, generators and tests.
Toolchains default to `/home/deck/Code/Toolchains/`; `ANDROID_HOME` and `ANDROID_NDK` can override the SDK/NDK paths.
Temporary files and Python/Java scratch data belong under `.work/totk-port/`, not system temporary directories.

Requirements include Java 21, Android SDK, NDK r30, CMake 4.1.2 and normal C/C++ build tools.
The build script creates the project Python environment and installs Capstone, Unicorn and LZ4.
Open-source dependencies require network access on the first build.

```bash
bash android/build_totk.sh
```

The default Debug/dev package is `.work/totk-port/packages/debug-dev/TOTKRecompAndroid-debug-dev-arm64-v8a.apk`.
Debug uses `com.totkrecomp.android.debug`, a Debug label and a blue DBG icon badge. Release uses
`com.totkrecomp.android` and the original icon. Historical Java/JNI directory names are retained.
The APK contains the runtime, not game code.
Ordinary Release disables native `TOTK_DIAGNOSTICS` and Java `BuildConfig.DIAGNOSTICS`;
Debug and the independent test package enable both. Capture backends, RenderDoc, probes and scripted test entry points
are excluded or disabled in ordinary Release. Rendering optimizations, shader preparation, normal input and the FPS display remain available.

### Build profiles

| `--profile` | Runtime and native hooks | Every generated game unit |
| --- | --- | --- |
| `dev` | O2 | O1, with cold/giant O0 policies available |
| `performance` | O3 + ThinLTO | O3 + ThinLTO; O0 exceptions and optnone are prohibited |
| `snapdragon-8elite` | O3 + ThinLTO, ARMv8.7-A + crypto + FP16, Oryon tuning | The same flags, including giant functions |

Release defaults to `performance`; selecting a performance profile alone selects Release. The 8 Elite profile uses
`-march=armv8.7-a+crypto+fp16 -mtune=oryon-1`. NDK r30's Clang 21 has no distinct second-generation Oryon scheduling
model. Performance profiles retain `-fno-fast-math` to preserve guest floating-point behavior.
Use `debug --profile dev` for everyday debugging and `release --profile performance` for a general Release.
The 8 Elite profile is device-specific. Both use NDK Clang; host Clang builds desktop tools/tests.
The native profile selects optimization independently of the diagnostics policy: ordinary Release disables diagnostics;
Debug and the independent test package enable them. Normal rendering feature defaults are shared.

```bash
bash android/build_totk.sh release --profile snapdragon-8elite
bash android/build_gamelib.sh .work/totk-port/game/exefs release --profile snapdragon-8elite
```

Each `.work/totk-port/packages/<variant>-<profile>/` directory contains its APK, the exact `libmain.so` extracted
from it, and the matching game library/sidecars. `build.json` records SHA256 hashes and the actual profile/GAMELIB_ID
read from stripped ELF exports. Packaging/deployment also read the actual binary APK manifest and reject stale
metadata, unintended application packages and incompatible profiles/IDs.

Add `--perftest` to both commands for `com.totkrecomp.android.perftest`; its directory gains a `-perftest` suffix.
Perftest Release keeps the debug signature and `run-as` while native CMake remains Release/O3. Ordinary Release is
not debuggable. `push_game.sh --perftest` explicitly targets this package. Changing profile requires both a new APK
and matching game library.
Ordinary Release uses `~/.totkrecompandroid/keystore.properties` when configured and otherwise falls back to the
debug key. Verify signing separately before publishing. Use `build_totk.sh` to stage packages and identity records;
direct Gradle builds do not refresh them. Serialize Gradle builds in one checkout: dev app/perftest still share intermediates.

On memory-limited hosts, set `JOBS=2 TOTK_LTO_JOBS=1` in dev. Native Ninja compiler pools also obey JOBS; linking is
serial. `TOTK_BUILD_LAUNCHER` can provide a compiler/linker resource-limit wrapper. NDK's LICM promotion workaround
applies to both compilation and ThinLTO backends and is part of the performance library identity. Phone Zig receives
an ARM feature set and direct Clang tuning arguments; `-O3 -flto=thin` selects real LLD O3 without unsupported Zig
linker CLI options.

## Prepare your game

Enter `distrobox enter dev`, then source the environment. Set the serial to the correct phone shown by `adb devices -l`;
other devices may be connected. Replace the example filenames and hactool location with your actual paths.

```bash
source android/dev_env.sh
export ANDROID_SERIAL="YOUR_ONEPLUS_ADB_SERIAL_OR_WIRELESS_ADDRESS"
"$PYTHON" tools/pull_totk.py \
  '/storage/emulated/0/Games/TotK/[APP][0100F2C0115B6000][1.0.0][US][16.0.0].nsp' \
  .work/totk-port/base --serial "$ANDROID_SERIAL"
"$PYTHON" tools/pull_totk.py \
  '/storage/emulated/0/Games/TotK/[UPD][0100F2C0115B6800][1.4.3][US][21.2.0].nsp' \
  .work/totk-port/update --serial "$ANDROID_SERIAL"
"$PYTHON" tools/prepare_totk.py \
  --base .work/totk-port/base --update .work/totk-port/update \
  --keys .work/totk-port/private/prod.keys \
  --hactool /home/deck/Code/Toolchains/hactool/hactool \
  --output .work/totk-port/game
```

Use keys from your own console. The local workflow used hactool 1.5.4. The preparation tool obtains title keys from your NSP tickets,
removes its temporary compatibility copies and leaves the original keyset unchanged. Phone files under `Games/TotK/` are read-only inputs.

Outputs are `game/exefs/`, merged `game/romfs.bin`, loose movies in `game/romfs/` and a manifest.
Streaming extraction avoids an additional local NSP copy, but allow space for roughly 16 GiB of NCA, 16 GiB of RomFS and build outputs.
Four full module build IDs and the NPDM program ID are checked. Other revisions are not accepted as 1.4.3.

## Game library and deployment

Build the APK first, because the generated library links against its runtime:

```bash
GAMELIB_GEN="$PWD/.work/totk-port/generated" \
bash android/build_gamelib.sh .work/totk-port/game/exefs debug --profile dev

"$PYTHON" tools/deploy_totk.py .work/totk-port/game \
  --library .work/totk-port/packages/debug-dev --serial "$ANDROID_SERIAL" \
  --apk .work/totk-port/packages/debug-dev/TOTKRecompAndroid-debug-dev-arm64-v8a.apk --launch
```

The library is `totkgame.so` with identity sidecars. Only dev uses O1/giant O0 policies. Performance profiles reject
`GAMELIB_FAST_COMPILE=ON` and all additional O0 exceptions.
See [phone performance tests](PHONE-PERFORMANCE.md) for measured NDK r30 compiler outliers.
`GAMELIB_DISABLE_LICM_PROMOTION` defaults to ON; `GAMELIB_EXTRA_HEAVY_SOURCES` accepts an explicit semicolon-separated
list of additional O0 units only in dev and defaults to empty. Record these build parameters with performance results.
On-phone resume signatures include compile/link flags and all four full 32-byte module build IDs.

Deployment uses the normal APK/Activity/JNI route. Data lives in `Android/data/<selected-package>/files/`, with separate
saves for Debug, Release and perftest. No helper is placed in `/data/local/tmp`. The foreground build service and phone
profile parameters are integrated; pinned Zig cross-compilation checks passed. Complete on-phone compilation remains
unvalidated and its compiler is not included in the current APK.

## Runtime-only updates

A change under `runtime/` usually needs only an APK rebuild:

```bash
# Keep the existing profile; choose the matching ordinary Debug or 8 Elite test package.
bash android/build_totk.sh debug --profile dev
bash android/build_totk.sh release --profile snapdragon-8elite --perftest
```

In `dev`, verify the existing library's profile/GAMELIB_ID, then install with
`adb -s "$ANDROID_SERIAL" install --streaming -r` followed by the exact staged APK path.
For tests, prefer `phone_test_session.py benchmark --apk` to install under the device lock.
`android/run_boot.sh` stops and restarts the application, captures logs for a bounded duration and leaves the game running afterward.
Use it for a required version update or a defined comparison; capture can otherwise be triggered in the running game.

## Validation and cleanup

Follow [testing](TESTING.md), and use `"$PYTHON" tools/totk.py .work/totk-port/game/exefs` to inspect prepared module identity.
The entire `.work/` tree is ignored by Git. Never commit keys, game data or generated game code.
After a successful deployment, obsolete `base/`, `update/`, `generated/` and `out/gamelib-build/` can be removed when no longer needed.
Retain required prepared inputs, library files, saves and valid investigation logs. Do not delete unanalyzed captures or the phone's original game files.
