# Dependency provenance

The package uses the separately built, minimal FFmpeg 9.0.2 binary with x264
commit b35605ace3ddf7c1a5d67a2eb553f034aef41d55. Its SHA-256 is
`efecdb39e828868bb71999dd3de914ba35d70aa535ca10e55d140a71d389dc34`
and its size is 6,032,398 bytes. It was built with `--enable-gpl` and
`--enable-libx264`; FFmpeg and x264 are licensed under GPL version 2 or later.
The accompanying FFmpeg-GPL-2.0.txt is the upstream FFmpeg 9.0.2 GPLv2 text.

Upstream: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz (SHA-256
`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`);
https://code.videolan.org/videolan/x264 at the commit above (the exported tar
SHA-256 is `e92edc35efe3f8c3effb0c04854de0f8428cc09a218b2062ec3fe44f8ad2aa4e`).

The pinned binary has no public hosting URL yet. A matching dependency source
archive and build recipe must be supplied before distributing a binary package;
the automatic Cinemania source archive does not contain these dependencies.
The packaged MinHook license is from https://github.com/TsudaKageyu/minhook,
version 1.3.4, commit c3fcafdc10146beb5919319d0683e44e3c30d537.
