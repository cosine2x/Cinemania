# Building and releasing Cinemania

## Publish a release

Update the project version in `CMakeLists.txt` and commit it with the changes
for the release. Push that commit to `master`, then push an annotated tag that
matches the project version. For example, for version `0.3.0`:

```powershell
git push origin master
git tag -a v0.3.0 -m "Cinemania 0.3.0"
git push origin v0.3.0
```

The **Publish Cinemania release** workflow runs for version tag pushes. It
builds the tagged source, packages the runtime and corresponding dependency
sources, and creates a GitHub release with generated notes. It verifies the
public downloads before pushing the new TMLoader feed entry to `master`.
Previous feed entries remain available.

If a publication step fails, rerun the failed workflow on the same tag.
The workflow can resume from a draft or use the verified assets of an already
published release. Once the feed is updated, install the release in TMLoader
to check that the package is offered and downloads successfully.

The workflow reuses the pinned FFmpeg binary and source inputs from the
`v0.1.0` release. When updating FFmpeg or x264, update the pinned download inputs
in `scripts/release.py` along with the matching package and source checksums.

## Build and package locally

Install Visual Studio 2022 or newer with the MSVC x86/x64 C++ tools and Windows
SDK, plus CMake 3.20 or newer, Ninja, Git and Python 3.10 or newer. Run from the repository root:

```powershell
./scripts/build.ps1
python ./scripts/package.py --ffmpeg "C:/path with spaces/ffmpeg.exe"
```

The build script discovers Visual Studio using vswhere and builds both Release
targets under `build/` and creates a receipt only after both builds succeed. If Python is not on PATH, pass its executable with `-Python "C:/path/python.exe"`. Packaging verifies the receipt, source fingerprint, Release configuration and binary hashes, then reads these outputs and writes a new
`out/Cinemania-<version>/` directory containing the ZIP and external SHA256SUMS.
To use another parent directory, pass `--output "C:/path with spaces/output"`.
Existing versioned output is never replaced; move it aside before running again.
`python ./scripts/package.py --help` lists the options. The package checks PE
architectures, the DLL ordinal 1 anchor, the FFmpeg size and pinned SHA-256,
and ZIP contents. Its manifest records the build receipt, including the source commit, dirty state, configuration and binary
hashes. ZIP ordering, metadata and manifest serialization are deterministic for
identical inputs; MSVC binaries themselves are not promised reproducible.

For local packaging, supply the trusted pinned FFmpeg executable explicitly.
It is included in the runtime ZIP from the latest release.
Before distributing the binary, create the matching dependency source bundle
from the pinned FFmpeg and x264 archives:

```powershell
$version = '0.3.0' # Use the CMake project version.
python ./scripts/source_bundle.py --ffmpeg-source "C:/path/ffmpeg-9.0.2.tar.xz" --x264-source "C:/path/x264-source.tar" --output "./out/Cinemania-$version/Cinemania-$version-dependency-sources.zip"
```

The script verifies both archive hashes and includes the build recipe. Upload
this ZIP as a release asset alongside the binary ZIP; the automatically generated
Cinemania source archive does not include these dependencies. See
`licenses/DEPENDENCIES.md` for hashes and provenance.

## Generate the TMLoader feed locally

After committing the release source, build and package from that clean commit.
Generate the feed from the resulting ZIP:

```powershell
python ./scripts/modloader_feed.py "./out/Cinemania-$version/Cinemania-$version.zip" --previous ./modloader/index.json
```

This writes a new `index.json` next to the ZIP, using its exact SHA-256. It
checks the archive layout, binary hashes and PE architecture, manifest version,
and clean source commit. Pass `--previous` to retain earlier releases.
Use `--output` to select a new output path. The generator refuses to
replace an existing version with different metadata. The entry depends on
`TmForever` 2.12.0, the supported executable baseline.

Close TMLoader and add the following entry under `servers:` in
its `config.yaml`, preserving any existing entries, then reopen TMLoader:

```yaml
servers:
  - https://raw.githubusercontent.com/mediocre-q/Cinemania/master/modloader/
```
