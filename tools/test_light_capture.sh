#!/usr/bin/env bash
# Requires an APK build, the debug package on the selected phone, and dev.
set -euo pipefail
cd "$(dirname "$0")/.."
source android/dev_env.sh
: "${ANDROID_SERIAL:?set the intended phone serial}"
: "${REPLAY_DRIVER:?set the installed custom Vulkan driver path on that phone}"
p="$TOTK_ROOT/.work/totk-port/light-capture-tests"
mkdir -p "$p"
b=$(python3 - <<'PY'
from pathlib import Path
paths=list(Path('android/botwrecompandroid/app/.cxx/Debug').glob('*/arm64-v8a/compile_commands.json'))
print(max(paths,key=lambda p:p.stat().st_mtime).parent.resolve())
PY
)
obj="$b/CMakeFiles/main.dir$TOTK_ROOT/runtime/nvn/vulkan"
volk=$(python3 - "$b" <<'PY'
from pathlib import Path
import sys
print(next(Path(sys.argv[1]).rglob('volk.c.o')))
PY
)
cc="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
cpp="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang++"
"$cc" -O2 -g -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -I. -Iandroid/deps/out/volk \
    -Iandroid/deps/out/include -Ithird_party/libadrenotools/include -c tools/test_light_capture.c -o "$p/check.o"
"$cpp" "$p/check.o" "$obj/flicker.c.o" "$obj/diagnostic.c.o" "$obj/renderdoc_bridge.c.o" \
    "$obj/vma.cpp.o" "$volk" "$b/adrenotools/libadrenotools.a" \
    "$b/adrenotools/lib/linkernsbypass/liblinkernsbypass.a" -Landroid/deps/out/lib \
    -lSDL3 -lz -llog -landroid -ldl -static-libstdc++ -Wl,-z,max-page-size=16384 -o "$p/check"
r=/data/user/0/com.totkrecomp.android/files/shader-replay/light-capture-tests
run="check-$(date +%Y%m%d-%H%M%S)"
adb -s "$ANDROID_SERIAL" shell "run-as com.totkrecomp.android mkdir -p $r" </dev/null
tar -C "$p" -cf "$p/input.tar" check
adb -s "$ANDROID_SERIAL" exec-in run-as com.totkrecomp.android tar -xf - -C "$r" < "$p/input.tar"
apk=$(adb -s "$ANDROID_SERIAL" shell pm path com.totkrecomp.android </dev/null)
apk=${apk#package:};apk=${apk%$'\r'};hooks=${apk%/base.apk}/lib/arm64
remote=$(python3 - "$hooks" "$REPLAY_DRIVER" "$r" "$run" <<'PY'
import shlex,sys
hooks,driver,root,run=sys.argv[1:]
print(shlex.join(['run-as','com.totkrecomp.android','env','LD_LIBRARY_PATH='+hooks,
                 'REPLAY_DRIVER='+driver,'REPLAY_HOOKS='+hooks,root+'/check',root+'/'+run]))
PY
)
adb -s "$ANDROID_SERIAL" shell "$remote" </dev/null > "$p/device.log" 2>&1
cat "$p/device.log"
adb -s "$ANDROID_SERIAL" exec-out run-as com.totkrecomp.android tar -C "$r" -cf - "$run" > "$p/results.tar"
mkdir -p "$p/results"
tar -xf "$p/results.tar" -C "$p/results"
"$PYTHON" tools/test_light_capture.py "$p/results/$run/flicker"
