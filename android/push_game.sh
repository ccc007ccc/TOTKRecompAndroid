#!/usr/bin/env bash
# Deploy prepared TotK inputs; no device /tmp helper.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$PWD/android/push_game.sh" "$@"
fi
source android/dev_env.sh
GAME=${1:?usage: android/push_game.sh <prepared game folder> [debug|release] [--profile PROFILE]}
shift
source android/build_profile.sh
totk_build_options "$@"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the target phone adb serial}"
exec "$PYTHON" tools/deploy_totk.py "$GAME" --variant "$TOTK_BUILD_VARIANT" \
    --package "$TOTK_PACKAGE" --profile "$TOTK_BUILD_PROFILE" \
    --library "${GAMELIB_OUT:-$TOTK_PACKAGE_DIR}" --serial "$ANDROID_SERIAL"
