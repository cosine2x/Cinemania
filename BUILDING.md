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

Normal builds do not build or download FFmpeg. Supply the trusted pinned
executable explicitly; it is included in the [0.1.0 runtime ZIP](https://github.com/mediocre-q/Cinemania/releases/download/v0.1.0/Cinemania-0.1.0.zip). CI
builds and uploads the DLL and worker only; it does not create a complete ZIP.
Before distributing the binary, create the matching dependency source bundle
from the pinned FFmpeg and x264 archives:

```powershell
python ./scripts/source_bundle.py --ffmpeg-source "C:/path/ffmpeg-9.0.2.tar.xz" --x264-source "C:/path/x264-source.tar" --output ./out/Cinemania-0.1.0/Cinemania-0.1.0-dependency-sources.zip
```

The script verifies both archive hashes and includes the build recipe. Upload
this ZIP as a release asset alongside the binary ZIP; the automatically generated
Cinemania source archive does not include these dependencies. See
`licenses/DEPENDENCIES.md` for hashes and provenance.

## TMLoader feed preparation

After committing the release source, build and package from that clean commit.
Generate the feed from the resulting ZIP:

```powershell
python ./scripts/modloader_feed.py ./out/Cinemania-0.1.0/Cinemania-0.1.0.zip
```

This writes a new `index.json` next to the ZIP, using its exact SHA-256. It
checks the archive layout, binary hashes and PE architecture, manifest version,
and clean source commit. To retain earlier releases, pass `--previous path/to/index.json`;
the output must be a new file (`--output` selects another path). It refuses to
replace an existing version with different metadata. The entry depends on
`TmForever` 2.12.0, the supported executable baseline. For 0.1.0, the runtime
asset URL and feed server base are respectively
`https://github.com/mediocre-q/Cinemania/releases/download/v0.1.0/Cinemania-0.1.0.zip`
and `https://raw.githubusercontent.com/mediocre-q/Cinemania/master/modloader/`.
For future versions, publish the release assets and verify their downloaded bytes
and redirects before updating `modloader/index.json` on `master`. Test installation
through TMLoader before announcing the feed; the generator does not change server
or game configuration.

Close TMLoader and add the following entry under `servers:` in
its `config.yaml`, preserving any existing entries, then reopen TMLoader:

```yaml
servers:
  - https://raw.githubusercontent.com/mediocre-q/Cinemania/master/modloader/
```
