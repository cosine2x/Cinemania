#!/usr/bin/env python3
"""Build a verified Cinemania package from explicit Release binaries.

Supply the trusted pinned FFmpeg binary via --ffmpeg. A separate dependency
source bundle is prepared by source_bundle.py.
This tool never downloads or builds FFmpeg.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
FFMPEG_SHA256 = "efecdb39e828868bb71999dd3de914ba35d70aa535ca10e55d140a71d389dc34"
FFMPEG_SIZE = 6032398
PACKAGE_LICENSE_FILES = {
    "LICENSE": "LICENSE",
    "licenses/MinHook-LICENSE.txt": "third_party/minhook/LICENSE.txt",
    "licenses/FFmpeg-GPL-2.0.txt": "licenses/FFmpeg-GPL-2.0.txt",
    "licenses/DEPENDENCIES.md": "licenses/DEPENDENCIES.md",
    "licenses/MinGW-w64-runtime.txt": "licenses/MinGW-w64-runtime.txt",
    "licenses/Winpthreads-LICENSE.txt": "licenses/Winpthreads-LICENSE.txt",
    "licenses/Winpthreads-source-notices.txt": "licenses/Winpthreads-source-notices.txt",
    "licenses/GCC-Runtime-Exception.txt": "licenses/GCC-Runtime-Exception.txt",
    "licenses/GCC-GPL-3.0.txt": "licenses/GCC-GPL-3.0.txt",
}
PACKAGE_ENTRIES = frozenset((*PACKAGE_LICENSE_FILES, "Cinemania.dll", "bin/CinemaniaWorker.exe", "bin/ffmpeg.exe", "manifest.json"))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def project_version():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Cinemania VERSION (\d+\.\d+\.\d+) LANGUAGES CXX\)", cmake)
    if not match:
        raise ValueError("CMake project version not found")
    return match.group(1)


def inspect_pe(data, machine, dll, anchor=False):
    def unpack(fmt, offset):
        try:
            return struct.unpack_from(fmt, data, offset)
        except struct.error as error:
            raise ValueError("truncated PE file") from error

    if data[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    pe = unpack("<I", 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    actual, sections, _, _, _, optional_size, flags = unpack("<HHIIIHH", pe + 4)
    if actual != machine or not flags & 0x2 or bool(flags & 0x2000) != dll:
        raise ValueError("unexpected PE architecture or DLL flag")
    optional = pe + 24
    magic = unpack("<H", optional)[0]
    if magic != (0x10b if machine == 0x14c else 0x20b):
        raise ValueError("unexpected PE optional header")
    minimum_optional = 104 if machine == 0x14c else 112
    section_table = optional + optional_size
    if optional_size < minimum_optional or section_table + sections * 40 > len(data) or not sections:
        raise ValueError("truncated PE optional header or section table")
    if not anchor:
        return

    export_rva, export_size = unpack("<II", optional + 96)

    def rva_offset(rva, length):
        for index in range(sections):
            section = section_table + index * 40
            virtual_size, address, raw_size, raw_offset = unpack("<IIII", section + 8)
            if address <= rva and rva + length <= address + min(virtual_size, raw_size):
                offset = raw_offset + rva - address
                if offset + length <= len(data):
                    return offset
        raise ValueError("invalid PE export address")

    if not export_rva or not export_size:
        raise ValueError("missing PE export table")
    table = rva_offset(export_rva, 40)
    base, function_count, name_count, functions, names, ordinals = unpack("<IIIIII", table + 16)
    if not (base <= 1 < base + function_count):
        raise ValueError("missing ordinal 1")
    for index in range(name_count):
        name_rva = unpack("<I", rva_offset(names + index * 4, 4))[0]
        ordinal = unpack("<H", rva_offset(ordinals + index * 2, 2))[0] + base
        name_offset = rva_offset(name_rva, 1)
        name_end = data.find(b"\0", name_offset)
        if ordinal == 1 and name_end >= 0 and data[name_offset:name_end] == b"CinemaniaLoadAnchor":
            function_rva = unpack("<I", rva_offset(functions + (ordinal - base) * 4, 4))[0]
            if function_rva and not export_rva <= function_rva < export_rva + export_size:
                rva_offset(function_rva, 1)
                return
    raise ValueError("ordinal 1 is not CinemaniaLoadAnchor")


def source_revision():
    def git(*args):
        return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()

    commit = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=normal"))
    return {"commit": commit, "dirty": dirty}



def source_fingerprint():
    paths = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    ).split(b"\0")
    relevant = ("src/", "third_party/", "scripts/")
    named = {"CMakeLists.txt", "CMakePresets.json"}
    digest = hashlib.sha256()
    for raw in sorted(set(paths)):
        if not raw:
            continue
        name = raw.decode("utf-8")
        if name not in named and not name.startswith(relevant):
            continue
        content = (ROOT / name).read_bytes()
        digest.update(len(raw).to_bytes(8, "little"))
        digest.update(raw)
        digest.update(len(content).to_bytes(8, "little"))
        digest.update(content)
    return digest.hexdigest()


def source_snapshot():
    return source_revision()["commit"] + ":" + source_fingerprint()


def release_config():
    expected = {
        "x86": {"CMAKE_BUILD_TYPE": "Release", "CINEMANIA_BUILD_WORKER": "OFF",
                "CMAKE_GENERATOR": "Ninja",
                "CMAKE_C_FLAGS_RELEASE": "/O2 /Ob2 /DNDEBUG",
                "CMAKE_CXX_FLAGS_RELEASE": "/O2 /Ob2 /DNDEBUG"},
        "x64": {"CMAKE_BUILD_TYPE": "Release", "CINEMANIA_BUILD_WORKER": "ON",
                "CMAKE_GENERATOR": "Ninja", "CMAKE_CXX_FLAGS_RELEASE": "/O2 /Ob2 /DNDEBUG"},
    }
    for arch, values in expected.items():
        cache = (ROOT / "build" / arch / "CMakeCache.txt").read_text(encoding="utf-8")
        if re.search(r"^CMAKE_(?:C|CXX)_FLAGS:STRING=.*(?:^|\s)/O(?:d|1|2|b[0123]|x)(?:\s|$)", cache, re.MULTILINE | re.IGNORECASE):
            raise ValueError(f"unexpected {arch} global optimization flags")
        for key, value in values.items():
            if not re.search(rf"^{key}:[^=]+={value}$", cache, re.MULTILINE):
                raise ValueError(f"unexpected {arch} CMake cache setting: {key}")
    return expected


def write_receipt(snapshot):
    if source_snapshot() != snapshot:
        raise ValueError("source changed during build; rebuild both targets")
    config = release_config()
    dll, worker = verify_build()
    receipt = {
        "source": source_revision(),
        "source_fingerprint_sha256": source_fingerprint(),
        "configuration": config,
        "binary_sha256": {"Cinemania.dll": sha256(dll), "CinemaniaWorker.exe": sha256(worker)},
    }
    path = ROOT / "build/receipt.json"
    path.write_text(json.dumps(receipt, indent=4, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def verified_receipt(dll, worker):
    receipt = json.loads((ROOT / "build/receipt.json").read_text(encoding="utf-8"))
    if receipt["source"]["commit"] != source_revision()["commit"]:
        raise ValueError("checkout commit differs from build receipt; rebuild")
    if receipt["source_fingerprint_sha256"] != source_fingerprint():
        raise ValueError("build sources differ from receipt; rebuild")
    if receipt["configuration"] != release_config():
        raise ValueError("build configuration differs from receipt; rebuild")
    hashes = {"Cinemania.dll": sha256(dll), "CinemaniaWorker.exe": sha256(worker)}
    if receipt["binary_sha256"] != hashes:
        raise ValueError("build binaries differ from receipt; rebuild")
    return receipt


def verify_build():
    dll = (ROOT / "build/x86/Cinemania.dll").read_bytes()
    worker = (ROOT / "build/x64/CinemaniaWorker.exe").read_bytes()
    inspect_pe(dll, 0x14c, True, anchor=True)
    inspect_pe(worker, 0x8664, False)
    return dll, worker


def package(ffmpeg_path, output_root):
    version = project_version()
    dll, worker = verify_build()
    receipt = verified_receipt(dll, worker)
    binaries = {
        "Cinemania.dll": dll,
        "bin/CinemaniaWorker.exe": worker,
        "bin/ffmpeg.exe": ffmpeg_path.read_bytes(),
    }
    ffmpeg = binaries["bin/ffmpeg.exe"]
    if len(ffmpeg) != FFMPEG_SIZE or sha256(ffmpeg) != FFMPEG_SHA256:
        raise ValueError("FFmpeg size or SHA-256 does not match the pinned binary")
    inspect_pe(ffmpeg, 0x8664, False)

    entries = dict(binaries)
    entries.update({name: (ROOT / source).read_bytes() for name, source in PACKAGE_LICENSE_FILES.items()})
    manifest = {
        "version": version,
        "build": receipt,
        "binaries": {
            name: {"sha256": sha256(content), "architecture": "x86" if name == "Cinemania.dll" else "x64"}
            for name, content in binaries.items()
        },
    }
    entries["manifest.json"] = (json.dumps(manifest, indent=4, sort_keys=True) + "\n").encode()

    output_root.mkdir(parents=True, exist_ok=True)
    destination = output_root / f"Cinemania-{version}"
    if destination.exists():
        raise FileExistsError(f"output already exists: {destination}")
    output_root = output_root.resolve()
    stage = Path(tempfile.mkdtemp(prefix=".cinemania-stage-", dir=output_root))
    try:
        archive = stage / f"Cinemania-{version}.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
            for name, content in sorted(entries.items()):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                zipped.writestr(info, content, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        with zipfile.ZipFile(archive) as zipped:
            if zipped.namelist() != sorted(entries) or zipped.testzip() is not None:
                raise ValueError("ZIP entries or CRC validation failed")
            for name, content in entries.items():
                if zipped.read(name) != content:
                    raise ValueError(f"ZIP content mismatch: {name}")
        checksum = f"{sha256(archive.read_bytes())}  {archive.name}\n"
        (stage / "SHA256SUMS.txt").write_text(checksum, encoding="ascii", newline="\n")
        os.rename(stage, destination)
    finally:
        if stage.exists():
            resolved_stage = stage.resolve()
            if resolved_stage.parent != output_root or not resolved_stage.name.startswith(".cinemania-stage-"):
                raise ValueError("refusing to clean staging directory outside output root")
            shutil.rmtree(resolved_stage)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", type=Path, help="path to the trusted pinned FFmpeg executable")
    parser.add_argument("--output", type=Path, default=ROOT / "out", help="parent directory for a new versioned output directory")
    parser.add_argument("--source-snapshot", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--write-receipt", metavar="SNAPSHOT", help=argparse.SUPPRESS)
    parser.add_argument("--verify-build", action="store_true", help="verify built DLL and worker without packaging")
    args = parser.parse_args()
    if not (args.verify_build or args.source_snapshot or args.write_receipt or args.ffmpeg):
        parser.error("--ffmpeg is required for packaging")
    try:
        if args.source_snapshot:
            print(source_snapshot())
        elif args.write_receipt:
            write_receipt(args.write_receipt)
            print("Build receipt written")
        elif args.verify_build:
            dll, worker = verify_build()
            verified_receipt(dll, worker)
            print("Build PE architecture and ordinal checks passed")
        else:
            print(package(args.ffmpeg, args.output))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
