#!/usr/bin/env bash
# Rebuild the pinned FFmpeg configuration in a fresh extracted bundle.
set -euxo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
printf '%s  %s\n' \
  8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e sources/ffmpeg-9.0.2.tar.xz \
  e92edc35efe3f8c3effb0c04854de0f8428cc09a218b2062ec3fe44f8ad2aa4e sources/x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar | sha256sum -c -
mkdir work
mkdir -p work/x264 work/bin work/prefix
tar -xf sources/ffmpeg-9.0.2.tar.xz -C work
tar -xf sources/x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar -C work/x264
# Match the original Strawberry toolchain selection. Override STRAWBERRY if needed.
STRAWBERRY="${STRAWBERRY:-/c/Strawberry}"
cp "$STRAWBERRY/c/bin/gmake.exe" work/bin/make.exe
export PATH="$ROOT/work/bin:$STRAWBERRY/c/bin:$STRAWBERRY/perl/bin:$PATH"
WINROOT="$(cygpath -m "$ROOT/work")"
export PKG_CONFIG_PATH="$ROOT/work/prefix/lib/pkgconfig"
cd "$ROOT/work/x264"
./configure --prefix="$WINROOT/prefix" --host=x86_64-w64-mingw32 --enable-static --disable-cli --disable-opencl --bit-depth=8 --chroma-format=420 --enable-pic
make -j16
make install
cd "$ROOT/work/ffmpeg-9.0.2"
./configure --prefix="$WINROOT/prefix" --pkg-config-flags=--static --extra-cflags="-I$WINROOT/prefix/include" --extra-ldflags="-L$WINROOT/prefix/lib -static" --extra-libs="-lpthread -lm" --enable-gpl --enable-libx264 --disable-autodetect --disable-everything --disable-doc --disable-debug --disable-ffplay --disable-ffprobe --disable-network --enable-static --disable-shared --enable-protocol=file,pipe --enable-demuxer=rawvideo,pcm_s16le,mov --enable-muxer=mp4 --enable-decoder=rawvideo,pcm_s16le,pcm_u8,wrapped_avframe --enable-encoder=libx264,aac --enable-filter=color,anullsrc,vflip,scale,format,setparams,asetpts,aresample --enable-indev=lavfi
make -j16
# The original build used this executable directly rather than make install.
printf 'Built executable: %s\n' "$ROOT/work/ffmpeg-9.0.2/ffmpeg.exe"
