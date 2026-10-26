#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${CONTAINER_ID:-} != dev && ! -f /run/.containerenv ]]; then
    exec distrobox enter dev -- bash "$PWD/android/build_totk.sh" "$@"
fi
source android/dev_env.sh
if [[ ! -x "$PYTHON" ]]; then
    python -m venv --system-site-packages .work/totk-port/venv
    PIP_CACHE_DIR="$TOTK_ROOT/.work/totk-port/pip-cache" "$PYTHON" -m pip install capstone==5.0.7 unicorn==2.1.4 lz4==4.4.5
fi
bash android/deps/build_deps.sh
if [[ ! -f android/deps/out/lib/libavformat.so ]]; then
    bash android/deps/build_ffmpeg.sh
fi
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers="$JOBS" assembleDebug
