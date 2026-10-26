#!/usr/bin/env bash
# Deploy prepared TotK inputs; no device /tmp helper.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$PWD/android/push_game.sh" "$@"
fi
source android/dev_env.sh
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the target phone adb serial}"
exec "$PYTHON" tools/deploy_totk.py "${1:?usage: android/push_game.sh <prepared game folder>}" \
    --library "${GAMELIB_OUT:-android/out}" --serial "$ANDROID_SERIAL"
