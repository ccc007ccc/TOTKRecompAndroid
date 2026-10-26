#!/bin/bash
# android/build_gamelib.sh <exefs folder>: totkgame.so for Android from the player's own game files - the game code
# generated with gen.py --dyn-base (into generated-android-dyn/, skipped when it's there; delete it to redo) and built
# with the NDK against the app's libmain.so (build the app first). Output: android/out/totkgame.so and totkgame.so.id,
# for <data root>/extracted/ on the device (android/push_game.sh), where the app picks them up.
set -euo pipefail
cd "$(dirname "$0")/.."
R=$PWD
EXEFS=${1:?usage: android/build_gamelib.sh <exefs folder (main, sdk, ...)>}
# (GAMELIB_GEN / GAMELIB_OUT: another game version beside the usual one, e.g. 1.6.0 in generated-android-dyn-160)
GEN=${GAMELIB_GEN:-$R/generated-android-dyn}
OUT=${GAMELIB_OUT:-$R/android/out}
W=$OUT/gamelib-build

if [[ ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$R/android/build_gamelib.sh" "$@"
fi
source "$R/android/dev_env.sh"
CMAKE_DIR=$ANDROID_HOME/cmake/4.1.2/bin
NDK=$ANDROID_NDK
EXE=
PY=$PYTHON
EXEFS=$(realpath "$EXEFS")
"$PY" "$R/tools/totk.py" "$EXEFS" > "$TMPDIR/input-manifest.json"

LIBMAIN=$(ls -t "$R"/android/botwrecompandroid/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so 2>/dev/null | head -1 || true)
[ -n "$LIBMAIN" ] || { echo "build the app first (android/botwrecompandroid: gradlew assembleDebug)"; exit 1; }

# The stamp binds all four game inputs and generator sources. A table file alone
# is not a completion marker after an interrupted or different-version generation.
mkdir -p "$GEN"
STAMP=$( { cat "$TMPDIR/input-manifest.json"; sha256sum tools/{gen,lifter,analyze,nso,passes,patches,simd_c}.py runtime/hle/hle_symbols.txt; } | sha256sum | cut -d' ' -f1)
OLD=$(cat "$GEN/input.id" 2>/dev/null || true)
if [[ "$STAMP" != "$OLD" ]]; then
    names=(main sdk subsdk0 rtld)
    bases=(0x7000000000 0x7004000000 0x7006000000 0x7007000000)
    for i in 0 1 2 3; do
        flags=()
        [[ ${names[i]} == sdk ]] && flags=(--hle "$R/runtime/hle/hle_symbols.txt")
        "$PY" "$R/tools/gen.py" "${names[i]}" "$EXEFS/${names[i]}" "${bases[i]}" "$GEN/${names[i]}" --simd asm --dyn-base "${flags[@]}"
    done
    echo "$STAMP" > "$GEN/input.id"
fi

# the files whose largest function has over 15000 labels: compiled at -O0 (android/gamelib/CMakeLists.txt, heavy.txt).
# v1.6.0: 4 files, 15k-34k labels - each took clang hours at -O1/-O2; the largest under it (14.5k) compiles normally
(cd "$GEN" && for f in main/*.c sdk/*.c subsdk0/*.c rtld/*.c; do
    awk -v f="$f" '/^[a-zA-Z].* f_[0-9a-f]+\(.*\) \{$/ {n = 0} /^L_[0-9a-f]+:/ {if (++n > 15000) {print f; exit}}' "$f"
done) > "$GEN/heavy.txt"
echo "heavy (at -O0): $(tr '\n' ' ' < "$GEN/heavy.txt")"

"$CMAKE_DIR/cmake$EXE" -S "$R/android/gamelib" -B "$W" -G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja$EXE" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release -DLAS_GEN="$GEN" -DLAS_LIBMAIN="$LIBMAIN" \
    -DTOTK_FAST_COMPILE="${GAMELIB_FAST_COMPILE:-OFF}"
"$CMAKE_DIR/cmake$EXE" --build "$W" -j"$JOBS"
mkdir -p "$OUT"
cp "$W/totkgame.so" "$W/totkgame.so.id" "$OUT/"
# (debug info off: ~580 MB -> ~110 MB to push and load; the symbols stay, for simpleperf and crash reports)
"$NDK/toolchains/llvm/prebuilt/$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)/bin/llvm-strip$EXE" --strip-debug "$OUT/totkgame.so"
ls -la "$OUT/totkgame.so" "$OUT/totkgame.so.id"

printf "143\n" > "$OUT/totkgame.so.game"
