#!/bin/bash
# android/deps/build_ffmpeg.sh: FFmpeg for Android arm64 into android/deps/out (the movie player, runtime/hle/movie.c,
# and the start screen's background frame) - an LGPL-only configuration: only what the
# game's movies need. FFmpeg's configure wants a Unix shell: on Windows run it in WSL
#   wsl -d Ubuntu-26.04 -- bash android/deps/build_ffmpeg.sh
# It uses the Linux NDK (the same version as the app; downloaded to ~/android-ndk-r30 when missing) and builds in
# ~/botwrecompandroid-ffmpeg (the Linux file system: much faster than /mnt). Shared libraries without version suffixes
# (libavcodec.so, not .so.62): the names an APK can carry.
set -euo pipefail
cd "$(dirname "$0")"
P=$PWD/out
NDK=${ANDROID_NDK_LINUX:-${ANDROID_NDK:-/home/deck/Code/Toolchains/android-ndk-r30}}
W=${FFMPEG_WORK:-$PWD/../../.work/totk-port/ffmpeg}
API=30
mkdir -p "$W" "$P/include" "$P/lib"

if [ ! -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]; then
    curl -fsSL -o "$W/ndk.zip" https://dl.google.com/android/repository/android-ndk-r30-linux.zip
    # (no unzip here, and Python's zipfile drops symlinks and modes - clang is a link to clang-NN: restored from the
    # entries' Unix attributes)
    python3 - "$W/ndk.zip" "$(dirname "$NDK")" <<'PY'
import os, stat, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    mode = i.external_attr >> 16
    path = os.path.join(sys.argv[2], i.filename)
    if stat.S_ISLNK(mode):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.lexists(path): os.remove(path)
        os.symlink(z.read(i).decode(), path)
    else:
        z.extract(i, sys.argv[2])
        if mode and not i.is_dir(): os.chmod(path, stat.S_IMODE(mode))
PY
    rm -f "$W/ndk.zip"
fi
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin

if [ ! -f "$W/ffmpeg-9.0.2.tar.xz" ]; then
    curl -fsSL -o "$W/ffmpeg-9.0.2.tar.xz.part" https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
    mv "$W/ffmpeg-9.0.2.tar.xz.part" "$W/ffmpeg-9.0.2.tar.xz"
fi
(cd "$W" && echo "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e  ffmpeg-9.0.2.tar.xz" | sha256sum -c --quiet)
rm -rf "$W/ffmpeg" "$W/prefix" && mkdir -p "$W/ffmpeg" && tar -xf "$W/ffmpeg-9.0.2.tar.xz" -C "$W/ffmpeg" --strip-components 1

cd "$W/ffmpeg"
./configure --prefix="$W/prefix" --enable-cross-compile --target-os=android --arch=aarch64 --cpu=armv8-a \
    --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
    --nm="$TC/llvm-nm" --ar="$TC/llvm-ar" --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
    --enable-shared --disable-static --enable-pic \
    --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
    --disable-swresample --disable-everything \
    --enable-protocol=file --enable-demuxer=matroska,mov \
    --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac
# unversioned shared library names (Android loads what the APK ships by exact file name)
sed -i -e 's/^SLIBNAME_WITH_MAJOR=.*/SLIBNAME_WITH_MAJOR=$(SLIBNAME)/' -e 's/^SLIBNAME_WITH_VERSION=.*/SLIBNAME_WITH_VERSION=$(SLIBNAME)/' \
       -e 's/^SLIB_INSTALL_NAME=.*/SLIB_INSTALL_NAME=$(SLIBNAME)/' -e 's/^SLIB_INSTALL_LINKS=.*/SLIB_INSTALL_LINKS=/' ffbuild/config.mak
make -j"${JOBS:-6}"
make install

for l in avcodec avformat avutil swscale; do cp "$W/prefix/lib/lib$l.so" "$P/lib/"; done
rm -rf "$P/include/libavcodec" "$P/include/libavformat" "$P/include/libavutil" "$P/include/libswscale"
cp -r "$W/prefix/include/"* "$P/include/"
echo "ffmpeg: $P"
ls -la "$P"/lib/libav*.so "$P"/lib/libswscale.so
