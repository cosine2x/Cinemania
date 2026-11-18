# FFmpeg dependency source and build recipe

This archive accompanies Cinemania's separately distributed x64 FFmpeg executable
(SHA-256 efecdb39e828868bb71999dd3de914ba35d70aa535ca10e55d140a71d389dc34,
6,032,398 bytes). It contains the unmodified FFmpeg 9.0.2 release and an
unmodified `git archive` export of x264 commit
b35605ace3ddf7c1a5d67a2eb553f034aef41d55. See `SHA256SUMS` for hashes.
Upstream sources: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz and
https://code.videolan.org/videolan/x264. The x264 export was derived from the
specified commit; it is not an independently signed upstream release archive.
FFmpeg's LICENSE.md and COPYING.GPLv2, and x264's COPYING, are in the source
tarballs. The resulting GPL-enabled build is GPL version 2 or later.

## Build

On x64 Windows, use Git for Windows Bash 5.2.37 with Unix utilities including
tar, sha256sum, cygpath, and a toolchain matching Strawberry Perl's WinLibs
`gcc-13.2.0-mingw-w64ucrt-11.0.1-r8`: GCC 13.2.0, MinGW-w64 11.0.1
(UCRT, POSIX threads), binutils 2.42, GNU Make 4.4.1 and NASM 2.16.01.
Perl and pkg-config are also required. The original toolchain was installed at
`C:/Strawberry`; set `STRAWBERRY` to another Git Bash path if necessary.
Unzip this archive into a fresh directory and run `bash build.sh`. The script
checks source hashes, extracts to a new `work` directory, builds and installs
x264 into `work/prefix`, then configures and builds FFmpeg. Use the resulting
`work/ffmpeg-9.0.2/ffmpeg.exe`; the original build did not run FFmpeg's install
step. Copy that executable to the desired installation location. No network is
needed after the listed build tools are installed.

A clean rebuild in the original build directory differed only in PE timestamps
and checksum. A different build path can cause further differences; byte-for-byte
reproduction is not promised.
The bundled x264 source export has no Git metadata, matching the shallow checkout
used for the original version generation (`0.165.x`).

## Runtime source audit

The original link uses `-static -lpthread` and the named WinLibs toolchain.
The `licenses` directory includes MinGW-w64 runtime and winpthreads notices,
and the GCC GPLv3 and Runtime Library Exception texts. MinGW-w64 and
winpthreads notices were taken from upstream v11.0.1. The recorded build used
GCC without a compiler plugin. Its runtime exception grants additional
permissions for eligible compilation processes; see the included license text.
Exact WinLibs patches and the linked-object inventory were not independently
verified. Windows system/UCRT DLLs are imported by the executable; no separately
built FFmpeg or x264 DLL is imported. Toolchain sources are not in this bundle.
