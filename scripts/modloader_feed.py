#!/usr/bin/env python3
"""Generate a TMLoader index from a finished Cinemania release ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
from package import FFMPEG_SHA256, FFMPEG_SIZE, PACKAGE_ENTRIES, inspect_pe, source_fingerprint

ROOT = Path(__file__).resolve().parent.parent
BASE = "https://github.com/mediocre-q/Cinemania"


def generate(archive, output, previous=None):
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Cinemania VERSION (\d+\.\d+\.\d+) LANGUAGES CXX\)", cmake)
    if not match:
        raise ValueError("CMake project version not found")
    version = match.group(1)
    if archive.name != f"Cinemania-{version}.zip":
        raise ValueError("ZIP filename does not match source version")
    with zipfile.ZipFile(archive) as zipped:
        names = zipped.namelist()
        if len(names) != len(set(names)) or set(names) != PACKAGE_ENTRIES or zipped.testzip() is not None:
            raise ValueError("unexpected ZIP entries or invalid CRC")
        manifest = json.loads(zipped.read("manifest.json"))
        if manifest["version"] != version:
            raise ValueError("ZIP manifest version does not match source version")
        source = manifest["build"]["source"]
        head = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
        dirty = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=normal"], text=True).strip()
        if source["dirty"] or source["commit"] != head or dirty:
            raise ValueError("ZIP was not built from the current clean source commit")
        if manifest["build"]["source_fingerprint_sha256"] != source_fingerprint():
            raise ValueError("ZIP build source fingerprint does not match checkout")
        for name in ("Cinemania.dll", "bin/CinemaniaWorker.exe", "bin/ffmpeg.exe"):
            data = zipped.read(name)
            if hashlib.sha256(data).hexdigest() != manifest["binaries"][name]["sha256"]:
                raise ValueError(f"ZIP manifest hash mismatch: {name}")
            if name == "bin/ffmpeg.exe" and (len(data) != FFMPEG_SIZE or hashlib.sha256(data).hexdigest() != FFMPEG_SHA256):
                raise ValueError("ZIP FFmpeg does not match the pinned binary")
            inspect_pe(data, 0x14c if name == "Cinemania.dll" else 0x8664,
                       name == "Cinemania.dll", anchor=name == "Cinemania.dll")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    feed = json.loads(previous.read_text(encoding="utf-8")) if previous else {
        "api": 1, "game": "TmForever", "products": {}}
    if feed.get("api") != 1 or feed.get("game") != "TmForever" or not isinstance(feed.get("products"), dict):
        raise ValueError("invalid previous TMLoader feed")
    product = feed["products"].setdefault("Cinemania", {
        "name": "Cinemania", "author": "mediocre-q", "type": "modification",
        "description": "Modern video export for TrackMania United Forever 2.12.0.",
        "homepage": BASE, "versions": {}})
    if not isinstance(product, dict) or product.get("name") != "Cinemania" or product.get("type") != "modification" or not isinstance(product.get("versions"), dict):
        raise ValueError("invalid Cinemania product in previous feed")
    entry = {"executable": "Cinemania.dll", "dependencies": [{"id": "TmForever", "version": "2.12.0"}],
             "download_link": f"{BASE}/releases/download/v{version}/Cinemania-{version}.zip",
             "hash_sha256": digest, "changelog": f"Cinemania {version}."}
    versions = product["versions"]
    if version in versions and versions[version] != entry:
        raise ValueError("version already exists with different metadata")
    versions[version] = entry
    # Exclusive creation avoids replacing a feed, ZIP, or other existing file.
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(feed, stream, indent=4, ensure_ascii=False)
        stream.write("\n")
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", type=Path, help="finished versioned release ZIP")
    parser.add_argument("--output", type=Path, help="new output file (default: index.json next to ZIP)")
    parser.add_argument("--previous", type=Path, help="existing feed to preserve when adding a version")
    args = parser.parse_args()
    try:
        output = args.output or args.zip.with_name("index.json")
        digest = generate(args.zip, output, args.previous)
        print(f"Wrote {output}; ZIP SHA-256: {digest}")
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"Feed generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
