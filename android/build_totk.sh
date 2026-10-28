#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${CONTAINER_ID:-} != dev && ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$PWD/android/build_totk.sh" "$@"
fi
source android/dev_env.sh
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    echo "Usage: android/build_totk.sh [debug|release] [--profile dev|performance|snapdragon-8elite] [--perftest]"
    exit 0
fi
source android/build_profile.sh
totk_build_options "$@"
if [[ ! -x "$PYTHON" ]]; then
    python -m venv --system-site-packages .work/totk-port/venv
    PIP_CACHE_DIR="$TOTK_ROOT/.work/totk-port/pip-cache" "$PYTHON" -m pip install capstone==5.0.7 unicorn==2.1.4 lz4==4.4.5
fi
bash android/deps/build_deps.sh
if [[ ! -f android/deps/out/lib/libavformat.so ]]; then
    bash android/deps/build_ffmpeg.sh
fi
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers="$JOBS" "assemble${TOTK_BUILD_VARIANT^}" \
    -PtotkBuildProfile="$TOTK_BUILD_PROFILE" -PtotkLtoJobs="${TOTK_LTO_JOBS:-2}" \
    -PperformanceTest="${TOTK_PERFORMANCE_TEST:-false}"
APK_BUILD="$TOTK_ROOT/android/botwrecompandroid/app/build"
if [[ $TOTK_BUILD_PROFILE != dev ]]; then
    APK_APP_DIR=app
    [[ ${TOTK_PERFORMANCE_TEST:-false} != true ]] || APK_APP_DIR=perftest
    APK_BUILD="$TOTK_ROOT/.work/totk-port/builds/$TOTK_BUILD_PROFILE/$APK_APP_DIR"
fi
"$PYTHON" "$TOTK_ROOT/tools/package_android.py" "$APK_BUILD/outputs/apk/$TOTK_BUILD_VARIANT/output-metadata.json" \
    --variant "$TOTK_BUILD_VARIANT" --profile "$TOTK_BUILD_PROFILE" --output "$TOTK_PACKAGE_DIR" --package "$TOTK_PACKAGE"
