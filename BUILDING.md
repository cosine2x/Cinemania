# Windows build and package

Install Visual Studio 2022 or newer with the MSVC x86/x64 C++ tools and Windows
SDK, plus CMake 3.20 or newer, Ninja, Git and Python 3.10 or newer. Run from the repository root:

```powershell
./scripts/build.ps1
python ./scripts/package.py --ffmpeg "C:/path with spaces/ffmpeg.exe"
```

The build script discovers Visual Studio using vswhere and builds both Release
targets under `build/` and creates a receipt only after both builds succeed. If Python is not on PATH, pass its executable with `-Python "C:/path/python.exe"`. Packaging verifies the receipt, source fingerprint, Release configuration and binary hashes, then reads these outputs and writes a new
`out/Cinemania-0.1.0/` directory containing the ZIP and external SHA256SUMS.
To use another parent directory, pass `--output "C:/path with spaces/output"`.
Existing versioned output is never replaced; move it aside before running again.
`python ./scripts/package.py --help` lists the options. The package checks PE
architectures, the DLL ordinal 1 anchor, the FFmpeg size and pinned SHA-256,
and ZIP contents. Its manifest records the build receipt, including the source commit, dirty state, configuration and binary
hashes. ZIP ordering, metadata and manifest serialization are deterministic for
identical inputs; MSVC binaries themselves are not promised reproducible.

Normal builds do not build or download FFmpeg. The minimal binary has no public
hosting location yet, so supply the trusted pinned executable explicitly. CI
builds and uploads the DLL and worker only; it does not create a complete ZIP.
Before any binary distribution, provide the matching dependency source archives
and build recipe; the automatically generated Cinemania source archive does not
include them. See `licenses/DEPENDENCIES.md` for hashes and provenance.
