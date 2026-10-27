# Build and deployment

[简体中文](../BUILD.md) | English

This guide targets TOTKRecompAndroid with Switch 1.4.3 inputs. See [status](STATUS.md) for current limitations.
Run commands from the project root unless stated otherwise.

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

Output: `android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk`.
Application ID: `com.totkrecomp.android`. Historical Java/JNI directory names are retained.
The APK contains the runtime, not game code.

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
GAMELIB_OUT="$PWD/.work/totk-port/out" \
bash android/build_gamelib.sh .work/totk-port/game/exefs

"$PYTHON" tools/deploy_totk.py .work/totk-port/game \
  --library .work/totk-port/out --serial "$ANDROID_SERIAL" \
  --apk android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk --launch
```

The library is `totkgame.so` with identity sidecars. Normal builds use `-O1`, with exceptionally large functions at `-O0`.
`GAMELIB_FAST_COMPILE=ON` sets all functions to `-O0` for diagnosis and is not a performance configuration.
See [phone performance tests](PHONE-PERFORMANCE.md) for measured NDK r30 compiler outliers.
`GAMELIB_DISABLE_LICM_PROMOTION` defaults to ON; `GAMELIB_EXTRA_HEAVY_SOURCES` accepts an explicit semicolon-separated
list of additional O0 units and defaults to empty. Record these build parameters with performance results.

Deployment uses the normal APK/Activity/JNI route. Data lives in `Android/data/com.totkrecomp.android/files/`, isolated from BOTW.
No helper is placed in `/data/local/tmp`. Complete on-phone compilation is not part of the currently validated workflow.

## Runtime-only updates

A change under `runtime/` usually needs only an APK rebuild:

```bash
distrobox enter dev -- bash <<'SH'
set -e
source android/dev_env.sh
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers=4 assembleDebug
SH
```

In `dev`, install with `adb -s "$ANDROID_SERIAL" install --streaming -r` followed by the APK path.
`android/run_boot.sh` stops and restarts the application, captures logs for a bounded duration and leaves the game running afterward.
Use it for a required version update or a defined comparison; capture can otherwise be triggered in the running game.

## Validation and cleanup

Follow [testing](TESTING.md), and use `"$PYTHON" tools/totk.py .work/totk-port/game/exefs` to inspect prepared module identity.
The entire `.work/` tree is ignored by Git. Never commit keys, game data or generated game code.
After a successful deployment, obsolete `base/`, `update/`, `generated/` and `out/gamelib-build/` can be removed when no longer needed.
Retain required prepared inputs, library files, saves and valid investigation logs. Do not delete unanalyzed captures or the phone's original game files.
