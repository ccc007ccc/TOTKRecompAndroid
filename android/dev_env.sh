#!/usr/bin/env bash
# Source after entering dev; all scratch files remain in the project.
TOTK_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export ANDROID_HOME=${ANDROID_HOME:-/home/deck/Code/Toolchains/android-sdk}
export ANDROID_NDK=${ANDROID_NDK:-/home/deck/Code/Toolchains/android-ndk-r30}
export TMPDIR="$TOTK_ROOT/.work/totk-port/scratch"
export TMP="$TMPDIR" TEMP="$TMPDIR"
mkdir -p "$TMPDIR"
if [[ -x /usr/lib/jvm/java-21-openjdk/bin/java ]]; then
    export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
    export PATH="$JAVA_HOME/bin:$PATH"
fi
export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS:-} -Djava.io.tmpdir=$TMPDIR -XX:-UsePerfData"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmake/4.1.2/bin:$PATH"
export PYTHON=${PYTHON:-$TOTK_ROOT/.work/totk-port/venv/bin/python}
export JOBS=${JOBS:-6}
