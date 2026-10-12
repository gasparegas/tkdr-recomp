#!/bin/sh
# Build a minimal FFmpeg (H.264 decoder + parser only) for the native and
# WebAssembly targets into third_party/ffmpeg/{native,web}. LGPL-2.1+.
set -e
VER=7.1.1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=$ROOT/third_party/ffmpeg
mkdir -p "$DIR"
cd "$DIR"
[ -d ffmpeg-$VER ] || curl -fsSL https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz | tar xJ
COMMON="--disable-everything --enable-decoder=h264 --enable-parser=h264 --disable-programs --disable-doc
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale
  --disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-securetransport
  --disable-videotoolbox --disable-audiotoolbox --disable-xlib --disable-sdl2 --disable-pthreads
  --disable-debug --enable-static --disable-shared"
build() {  # $1 = name, rest = extra configure args
  name=$1; shift
  [ -f "$DIR/$name/lib/libavcodec.a" ] && return
  rm -rf "$DIR/build-$name" && mkdir -p "$DIR/build-$name" && cd "$DIR/build-$name"
  "$@" ../ffmpeg-$VER/configure --prefix="$DIR/$name" $COMMON $EXTRA
  make -j8 >/dev/null && make install >/dev/null
  cd "$DIR"
}
EXTRA="" build native env
EXTRA="--cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm --target-os=none --arch=x86_32
  --enable-cross-compile --disable-asm --disable-stripping --extra-cflags=-pthread" build web emconfigure
echo "ffmpeg ready in $DIR/{native,web}"
