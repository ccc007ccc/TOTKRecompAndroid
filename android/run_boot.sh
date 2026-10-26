#!/usr/bin/env bash
# Normal APK startup with a bounded log capture; no global logcat clearing.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$PWD/android/run_boot.sh" "$@"
fi
source android/dev_env.sh
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the target phone adb serial}"
SECS=${1:-30}
BOOT_ENV=LARECOMP_NO_LAUNCHER=1${2:+,$2}
OUT="$TOTK_ROOT/.work/totk-port/logs/boot.log"
mkdir -p "$(dirname "$OUT")"
adb shell am force-stop com.totkrecomp.android
timeout "$SECS" adb logcat -b all -v threadtime -T 1 TOTKRecompAndroid:V SDL:V DEBUG:V libc:F AndroidRuntime:E '*:S' > "$OUT" &
LOG_PID=$!
trap 'kill "$LOG_PID" 2>/dev/null || true' EXIT
adb shell am start -n com.totkrecomp.android/com.botwrecompandroid.BOTWRecompAndroidActivity --es env "$BOOT_ENV"
wait "$LOG_PID" || true
trap - EXIT
tail -40 "$OUT"
