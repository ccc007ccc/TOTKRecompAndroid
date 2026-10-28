#!/bin/bash
# android/build_gamelib.sh <exefs folder>: totkgame.so for Android from the player's own game files - the game code
# generated with gen.py --dyn-base and built with the NDK against the selected APK's libmain.so.
# Profile packages stay separate under .work/totk-port/packages/; build the matching app first.
set -euo pipefail
cd "$(dirname "$0")/.."
R=$PWD
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    echo "Usage: android/build_gamelib.sh <exefs folder> [debug|release] [--profile dev|performance|snapdragon-8elite]"
    exit 0
fi
EXEFS=${1:?usage: android/build_gamelib.sh <exefs folder> [debug|release] [--profile PROFILE]}

if [[ ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$R/android/build_gamelib.sh" "$@"
fi
source "$R/android/dev_env.sh"
shift
source "$R/android/build_profile.sh"
totk_build_options "$@"
GEN=${GAMELIB_GEN:-$R/.work/totk-port/generated}
OUT=${GAMELIB_OUT:-$TOTK_PACKAGE_DIR}
GAMELIB_FAST_COMPILE=${GAMELIB_FAST_COMPILE:-OFF}
if [[ $TOTK_BUILD_PROFILE != dev ]]; then
    case "${GAMELIB_FAST_COMPILE^^}" in ON|TRUE|YES|Y|1) echo "GAMELIB_FAST_COMPILE is incompatible with a performance profile" >&2; exit 2 ;; esac
fi
CMAKE_DIR=$ANDROID_HOME/cmake/4.1.2/bin
NDK=$ANDROID_NDK
EXE=
PY=$PYTHON
EXEFS=$(realpath "$EXEFS")
INPUT_MANIFEST=$(mktemp "$TMPDIR/totk-input.XXXXXX.json")
trap 'rm -f "$INPUT_MANIFEST"' EXIT
"$PY" "$R/tools/totk.py" "$EXEFS" > "$INPUT_MANIFEST"

LIBMAIN=${GAMELIB_LIBMAIN:-$TOTK_PACKAGE_DIR/libmain.so}
[[ -f $LIBMAIN ]] || { echo "build the matching app first: bash android/build_totk.sh $TOTK_BUILD_VARIANT --profile $TOTK_BUILD_PROFILE" >&2; exit 1; }
LIBMAIN=$(realpath "$LIBMAIN")

# The stamp binds all four game inputs and generator sources. A table file alone
# is not a completion marker after an interrupted or different-version generation.
mkdir -p "$GEN" "$OUT"
GEN=$(realpath "$GEN")
OUT=$(realpath "$OUT")
W=$OUT/gamelib-build
STAMP=$( { cat "$INPUT_MANIFEST"; sha256sum tools/{gen,lifter,analyze,nso,passes,patches,simd_c}.py runtime/hle/hle_symbols.txt; } | sha256sum | cut -d' ' -f1)
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

# In dev only, files whose largest function has over 15000 labels are compiled at -O0.
# v1.6.0: 4 files, 15k-34k labels - each took clang hours at -O1/-O2; the largest under it (14.5k) compiles normally
if [[ $TOTK_BUILD_PROFILE == dev ]]; then
(cd "$GEN" && for f in main/*.c sdk/*.c subsdk0/*.c rtld/*.c; do
    awk -v f="$f" '/^[a-zA-Z].* f_[0-9a-f]+\(.*\) \{$/ {n = 0} /^L_[0-9a-f]+:/ {if (++n > 15000) {print f; exit}}' "$f"
done) > "$GEN/heavy.txt"
echo "heavy (at -O0): $(tr '\n' ' ' < "$GEN/heavy.txt")"
else
    echo "performance build: every generated unit at -O3, including heavy.txt entries"
fi

"$CMAKE_DIR/cmake$EXE" -S "$R/android/gamelib" -B "$W" -G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja$EXE" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release -DLAS_GEN="$GEN" -DLAS_LIBMAIN="$LIBMAIN" \
    -DTOTK_FAST_COMPILE="${GAMELIB_FAST_COMPILE:-OFF}" -DTOTK_BUILD_PROFILE="$TOTK_BUILD_PROFILE" \
    -DTOTK_DISABLE_LICM_PROMOTION="${GAMELIB_DISABLE_LICM_PROMOTION:-ON}" \
    -DTOTK_EXTRA_HEAVY_SOURCES="${GAMELIB_EXTRA_HEAVY_SOURCES:-}" \
    -DTOTK_LTO_JOBS="${TOTK_LTO_JOBS:-2}" -DTOTK_COMPILE_JOBS="$JOBS" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    ${TOTK_BUILD_LAUNCHER:+"-DCMAKE_C_COMPILER_LAUNCHER=$TOTK_BUILD_LAUNCHER"} \
    ${TOTK_BUILD_LAUNCHER:+"-DCMAKE_C_LINKER_LAUNCHER=$TOTK_BUILD_LAUNCHER"}
"$CMAKE_DIR/cmake$EXE" --build "$W" -j"$JOBS"
mkdir -p "$OUT"
cp "$W/totkgame.so" "$W/totkgame.so.id" "$W/totkgame.so.profile" "$OUT/"
# (debug info off: ~580 MB -> ~110 MB to push and load; the symbols stay, for simpleperf and crash reports)
"$NDK/toolchains/llvm/prebuilt/$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)/bin/llvm-strip$EXE" --strip-debug "$OUT/totkgame.so"
ls -la "$OUT/totkgame.so" "$OUT/totkgame.so.id"

printf "143\n" > "$OUT/totkgame.so.game"
