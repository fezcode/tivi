#!/bin/sh
# Build tivi on macOS — produces build/tivi.
# Mirrors build.ps1: raylib + FFmpeg (libav*) + libass via pkg-config
# (Homebrew: brew install raylib ffmpeg libass pkgconf). The Win32-only
# modules (osvideo dialogs/DWM, mediakeys, singleinst) compile as stubs;
# hardware decode falls back to FFmpeg's software path.
set -e
cd "$(dirname "$0")"

PKGS="raylib libavformat libavcodec libavutil libswscale libswresample libavfilter libass"
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc $(pkg-config --cflags $PKGS)"
# yuvtex.c calls gl* directly, so the OpenGL framework is linked explicitly
# (raylib's own GL use resolves inside its dylib and doesn't cover us).
# Cocoa/MediaPlayer/UniformTypeIdentifiers back the *_mac.m parity modules.
LIBS="$(pkg-config --libs $PKGS) -framework OpenGL -lm -lpthread \
      -framework Cocoa -framework MediaPlayer -framework UniformTypeIdentifiers"

mkdir -p build
SRCS="main player audio_out subs playlist mediakeys singleinst osvideo viconfig yuvtex"
OBJC_SRCS="osvideo_mac mediakeys_mac"

OBJS=""
for s in $SRCS; do
    echo "compiling $s"
    cc $CFLAGS -c "src/$s.c" -o "build/$s.o"
    OBJS="$OBJS build/$s.o"
done
for s in $OBJC_SRCS; do
    echo "compiling $s (objc)"
    cc $CFLAGS -fobjc-arc -c "src/$s.m" -o "build/$s.o"
    OBJS="$OBJS build/$s.o"
done

echo 'linking build/tivi'
cc $OBJS -o build/tivi $LIBS
echo "built $(pwd)/build/tivi"
