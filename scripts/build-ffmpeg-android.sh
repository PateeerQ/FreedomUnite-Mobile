#!/usr/bin/env bash
# FFmpeg for the Android build: only what the game uses -- the H.264 decoder
# and parser (movies), ATRAC3 and ATRAC3+ (music, movie sound), swscale and
# swresample -- as static libraries for arm64-v8a, into deps/ffmpeg-android
# (the HEVC parser too: in 7.1 the H.264 decoder's SEI code calls into
# aom_film_grain.c, which only the HEVC parts build)
# (include/, lib/), where CMakeLists.txt looks on Android. LGPL, no programs.
#
# Run from Git Bash on Windows (the NDK's own clang and make do the work):
#   scripts/build-ffmpeg-android.sh [path/to/ffmpeg-source]
# The source defaults to deps/ffmpeg-src (FFmpeg 7.1.1, the same as the
# Windows build: https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:-$repo/deps/ffmpeg-src}"
out="$repo/deps/ffmpeg-android"
ndk="${ANDROID_NDK:-$LOCALAPPDATA/Android/Sdk/ndk/27.0.12077973}"
ndk="$(cygpath -u "$ndk" 2>/dev/null || echo "$ndk")"
api=26
tc="$ndk/toolchains/llvm/prebuilt/windows-x86_64"
make="$ndk/prebuilt/windows-x86_64/bin/make.exe"
target="aarch64-linux-android$api"

[ -f "$src/configure" ] || { echo "no FFmpeg source in $src"; exit 1; }
[ -x "$tc/bin/clang.exe" ] || { echo "no NDK clang in $tc"; exit 1; }

build="$repo/deps/ffmpeg-android-build"
# Emptied rather than removed: Windows will not remove a folder a shell is in.
mkdir -p "$build" && rm -rf "${build:?}"/* && mkdir -p "$build/tmp" && cd "$build"
# configure's scratch files: Git Bash inherits a Windows TEMP it cannot use.
export TMPDIR="$build/tmp"

"$src/configure" \
    --prefix="$out" \
    --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
    --cc="$tc/bin/clang.exe" --cxx="$tc/bin/clang++.exe" --ld="$tc/bin/clang.exe" \
    --ar="$tc/bin/llvm-ar.exe" --nm="$tc/bin/llvm-nm.exe" --ranlib="$tc/bin/llvm-ranlib.exe" \
    --strip="$tc/bin/llvm-strip.exe" \
    --sysroot="$tc/sysroot" \
    --host-cc="$tc/bin/clang.exe" \
    --host-cflags="--target=$target --sysroot=$tc/sysroot" \
    --host-ldflags="--target=$target --sysroot=$tc/sysroot" \
    --extra-cflags="--target=$target -O2 -fPIC" \
    --extra-ldflags="--target=$target" \
    --enable-static --disable-shared --enable-pic \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-network \
    --disable-everything \
    --enable-decoder=h264,atrac3,atrac3al,atrac3p,atrac3pal \
    --enable-parser=h264,hevc \
    --enable-swscale --enable-swresample

# The NDK's make is a Windows program: it needs E:/... where configure wrote
# Git Bash's /e/... paths.
for p in "$repo" "$ndk"; do
    win="$(cygpath -m "$p")"
    sed -i "s#$p#$win#g" Makefile ffbuild/config.mak
done

"$make" -j"$(nproc 2>/dev/null || echo 8)"
# install trips over its own long command lines for libavutil's headers on
# Windows; whatever it misses is copied after it.
"$make" install || true
for lib in libavcodec libavutil libswresample libswscale; do
    mkdir -p "$out/include/$lib"
    headers="$(sed -n '/^HEADERS *=/,/^$/p' "$src/$lib/Makefile" | tr -d '\\' | tr ' ' '\n' | grep '\.h$' || true)"
    for h in $headers; do
        [ -f "$out/include/$lib/$h" ] || cp "$src/$lib/$h" "$out/include/$lib/" 2>/dev/null || true
    done
done
for h in avconfig.h ffversion.h; do
    [ -f "$out/include/libavutil/$h" ] || cp "$build/libavutil/$h" "$out/include/libavutil/"
done
echo "FFmpeg for arm64-v8a in $out"
