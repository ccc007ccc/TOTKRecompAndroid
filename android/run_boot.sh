#!/usr/bin/env bash
# Normal APK startup with a bounded log capture; no global logcat clearing.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$PWD/android/run_boot.sh" "$@"
fi
source android/dev_env.sh
source android/build_profile.sh
totk_build_options
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the target phone adb serial}"
SECS=${1:-30}
BOOT_ENV=LARECOMP_NO_LAUNCHER=1${2:+,$2}
OUT="$TOTK_ROOT/.work/totk-port/logs/boot-$TOTK_BUILD_VARIANT.log"
mkdir -p "$(dirname "$OUT")"
adb -s "$ANDROID_SERIAL" shell am force-stop "$TOTK_PACKAGE"
timeout "$SECS" adb -s "$ANDROID_SERIAL" logcat -b all -v threadtime -T 1 TOTKRecompAndroid:V SDL:V DEBUG:V libc:F AndroidRuntime:E '*:S' > "$OUT" &
LOG_PID=$!
trap 'kill "$LOG_PID" 2>/dev/null || true' EXIT
adb -s "$ANDROID_SERIAL" shell am start -n "$TOTK_PACKAGE/com.botwrecompandroid.BOTWRecompAndroidActivity" --es env "$BOOT_ENV"
wait "$LOG_PID" || true
trap - EXIT
tail -40 "$OUT"
