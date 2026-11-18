#!/usr/bin/env python3
"""Build the release archives and publish the TMLoader feed for a version tag."""

import argparse
from io import BytesIO
import json
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
from urllib.error import URLError
from urllib.request import urlopen
from zipfile import BadZipFile, ZipFile

sys.dont_write_bytecode = True
import modloader_feed
import package
from source_bundle import SOURCES

ROOT = package.ROOT
FEED = Path("modloader/index.json")
MASTER = "refs/remotes/origin/master"
DEPENDENCY_RELEASE = "v0.1.0"
DEPENDENCY_ARCHIVES = {
    "Cinemania-0.1.0.zip": "ddf8d5a3dc0059339e498fc015b716ea8cb0b9576f4d9c52aca6c71131c05f30",
    "Cinemania-0.1.0-dependency-sources.zip": "fa6cce44ab05da645646bd0224b7ce78a2b9bb2b6aaa9571a78f0fa4118f85ed",
}


def output(*args):
    return subprocess.check_output(args, cwd=ROOT)


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def validate_tag(tag):
    if not re.fullmatch(r"v(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}", tag):
        raise ValueError("expected a stable release tag such as v0.3.0")
    if tag != f"v{package.project_version()}":
        raise ValueError("tag does not match the CMake project version")
    tag_commit, head = output("git", "rev-parse", f"refs/tags/{tag}^{{commit}}", "HEAD").splitlines()
    if tag_commit != head or output("git", "status", "--porcelain"):
        raise ValueError("expected a clean checkout of the release tag")
    run("git", "merge-base", "--is-ancestor", "HEAD", MASTER)


def download(tag, name, expected_digest):
    url = f"{modloader_feed.BASE}/releases/download/{tag}/{name}"
    for attempt in range(3):
        try:
            with urlopen(url, timeout=45) as response:
                data = response.read()
            break
        except (URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(2 ** attempt)
    if package.sha256(data) != expected_digest:
        raise ValueError(f"SHA-256 mismatch for {url}")
    return data


def asset_names(version):
    stem = f"Cinemania-{version}"
    return f"{stem}.zip", f"{stem}-dependency-sources.zip", "SHA256SUMS.txt"


def checksum_text(archives):
    return "".join(f"{package.sha256(path.read_bytes())}  {path.name}\n" for path in archives)


def build_archives(version):
    runtime, sources, _ = asset_names(DEPENDENCY_RELEASE[1:])
    with TemporaryDirectory() as temporary:
        inputs = Path(temporary)
        ffmpeg = inputs / "ffmpeg.exe"
        with ZipFile(BytesIO(download(DEPENDENCY_RELEASE, runtime, DEPENDENCY_ARCHIVES[runtime]))) as archive:
            ffmpeg.write_bytes(archive.read("bin/ffmpeg.exe"))
        with ZipFile(BytesIO(download(DEPENDENCY_RELEASE, sources, DEPENDENCY_ARCHIVES[sources]))) as archive:
            for name, _ in SOURCES:
                (inputs / name).write_bytes(archive.read(f"sources/{name}"))

        folder = package.package(ffmpeg, ROOT / "out")
        runtime, sources, checksums = (folder / name for name in asset_names(version))
        run(sys.executable, ROOT / "scripts/source_bundle.py", "--ffmpeg-source", inputs / SOURCES[0][0],
            "--x264-source", inputs / SOURCES[1][0], "--output", sources)
        checksums.write_text(checksum_text((runtime, sources)), encoding="ascii", newline="\n")


def release_info(tag):
    result = subprocess.run(
        ("gh", "release", "view", tag, "--json", "isDraft,assets"),
        cwd=ROOT, capture_output=True, text=True,
    )
    if result.returncode and result.stderr.strip() == "release not found":
        return None
    if result.returncode:
        raise ValueError(f"could not inspect {tag}: {result.stderr.strip()}")
    return json.loads(result.stdout)


def asset_digests(release):
    return {
        asset["name"]: asset["digest"].removeprefix("sha256:")
        for asset in release["assets"] if asset["state"] == "uploaded" and asset["digest"]
    }


def publish_assets(tag, release, paths):
    if release is None:
        run("gh", "release", "create", tag, "--verify-tag", "--draft", "--generate-notes",
            "--title", f"Cinemania {tag[1:]}", *paths)
    elif release["isDraft"]:
        run("gh", "release", "upload", tag, "--clobber", *paths)
    else:
        return asset_digests(release)

    uploaded = release_info(tag)
    expected = {path.name: package.sha256(path.read_bytes()) for path in paths}
    if not uploaded or not uploaded["isDraft"] or asset_digests(uploaded) != expected:
        raise ValueError("uploaded draft assets do not match the release packages")
    run("gh", "release", "edit", tag, "--draft=false", "--latest")
    return expected


def publish(tag):
    version = tag[1:]
    run("git", "fetch", "origin", f"refs/heads/master:{MASTER}")
    run("git", "merge-base", "--is-ancestor", "HEAD", MASTER)
    master = output("git", "rev-parse", MASTER).decode().strip()
    previous = output("git", "show", f"{master}:{FEED.as_posix()}")
    versions = json.loads(previous)["products"]["Cinemania"]["versions"]
    release = release_info(tag)
    if version in versions and (release is None or release["isDraft"]):
        raise ValueError("the feed already advertises this version without a public release")
    if version not in versions and any(
        tuple(map(int, version.split("."))) <= tuple(map(int, published.split(".")))
        for published in versions
    ):
        raise ValueError("the tag must be newer than the versions in the TMLoader feed")

    names = asset_names(version)
    digests = publish_assets(tag, release, [ROOT / "out" / f"Cinemania-{version}" / name for name in names])
    with TemporaryDirectory() as temporary:
        work = Path(temporary)
        runtime, sources, checksums = (work / name for name in names)
        for path in (runtime, sources, checksums):
            path.write_bytes(download(tag, path.name, digests[path.name]))
        if checksums.read_text(encoding="ascii") != checksum_text((runtime, sources)):
            raise ValueError("public release archives do not match SHA256SUMS.txt")

        old_feed, new_feed = work / "previous.json", work / "index.json"
        old_feed.write_bytes(previous)
        modloader_feed.generate(runtime, new_feed, old_feed)
        if new_feed.read_bytes() == previous:
            print(f"TMLoader feed already contains {version}.")
            return

        run("git", "switch", "--detach", master)
        (ROOT / FEED).write_bytes(new_feed.read_bytes())
        run("git", "add", "--", FEED)
        run("git", "-c", "user.name=github-actions[bot]", "-c",
            "user.email=41898282+github-actions[bot]@users.noreply.github.com",
            "commit", "-m", f"build: publish the ModLoader feed for {version}")
        run("git", "push", "origin", "HEAD:refs/heads/master")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("step", choices=("validate", "package", "publish"))
    parser.add_argument("tag")
    args = parser.parse_args()
    try:
        validate_tag(args.tag)
        if args.step == "package":
            build_archives(args.tag[1:])
        elif args.step == "publish":
            publish(args.tag)
    except (OSError, ValueError, KeyError, BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release failed: {error}\n")


if __name__ == "__main__":
    main()
