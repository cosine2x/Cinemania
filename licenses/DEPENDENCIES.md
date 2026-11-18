# Dependency provenance

The package uses a separately built minimal x64 FFmpeg 9.0.2 executable with
x264 commit b35605ace3ddf7c1a5d67a2eb553f034aef41d55. Its SHA-256 is
`efecdb39e828868bb71999dd3de914ba35d70aa535ca10e55d140a71d389dc34`
and its size is 6,032,398 bytes. It was built with `--enable-gpl` and
`--enable-libx264`; FFmpeg and x264 are licensed under GPL version 2 or later.
The accompanying FFmpeg-GPL-2.0.txt is FFmpeg's GPLv2 text.

The separate FFmpeg/x264 source bundle contains the original FFmpeg archive,
x264 source export, their upstream license texts, and the build recipe. It is
prepared offline by `scripts/source_bundle.py` and must be made available
alongside the executable; the Cinemania source archive and runtime ZIP do not
contain these sources. FFmpeg upstream:
https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, SHA-256
`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`.
x264 upstream: https://code.videolan.org/videolan/x264 at the above commit,
exported tar SHA-256
`e92edc35efe3f8c3effb0c04854de0f8428cc09a218b2062ec3fe44f8ad2aa4e`.

The build used WinLibs GCC 13.2.0 / MinGW-w64 11.0.1 UCRT r8, with
`-static -lpthread`. Static MinGW-w64 runtime and winpthreads notices are
included, based on the upstream v11.0.1 sources (tag commit
`c3e587c067a00a561899d49d3e63a659e38802ec`). GCC runtime components may also
be linked; GPLv3 and the GCC Runtime Library Exception are included. The GCC
exception provides additional permissions for eligible compilation processes;
see the included text. These permissive MinGW-w64 and winpthreads notices include their copyright
and binary redistribution terms. The recorded build used GCC without a compiler
plugin. The bundle does not include toolchain source. The exact WinLibs patches
and linked-object inventory have not been independently verified.

The packaged MinHook license is from https://github.com/TsudaKageyu/minhook,
version 1.3.4, commit c3fcafdc10146beb5919319d0683e44e3c30d537.
