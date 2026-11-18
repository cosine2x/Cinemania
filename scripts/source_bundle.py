#!/usr/bin/env python3
"""Create the offline FFmpeg/x264 source bundle from verified input archives."""

import argparse
import hashlib
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parent / "dependency-source"
SOURCES = (
    ("ffmpeg-9.0.2.tar.xz", "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"),
    ("x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar", "e92edc35efe3f8c3effb0c04854de0f8428cc09a218b2062ec3fe44f8ad2aa4e"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg-source", type=Path, required=True)
    parser.add_argument("--x264-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    entries = []
    for (name, expected), path in zip(SOURCES, (args.ffmpeg_source, args.x264_source)):
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"SHA-256 mismatch: {path}")
        entries.append(("sources/" + name, data))
    entries.extend((name, (ROOT / name).read_bytes()) for name in ("README.md", "build.sh", "SHA256SUMS"))
    notices = ("MinGW-w64-runtime.txt", "Winpthreads-LICENSE.txt",
               "Winpthreads-source-notices.txt", "GCC-Runtime-Exception.txt", "GCC-GPL-3.0.txt")
    entries.extend(("licenses/" + name, (ROOT.parent.parent / "licenses" / name).read_bytes()) for name in notices)
    # Exclusive creation protects existing files, including source inputs.
    with args.output.open("xb") as output:
        try:
            with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
                for name, data in sorted(entries):
                    info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                    info.create_system = 3
                    info.external_attr = (0o100644 << 16)
                    info.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(info, data, compresslevel=9)
        except BaseException:
            output.close()
            args.output.unlink()
            raise
    print(f"Wrote {args.output} (SHA-256 {hashlib.sha256(args.output.read_bytes()).hexdigest()})")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
