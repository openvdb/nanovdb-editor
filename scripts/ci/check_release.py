#!/usr/bin/env python3
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import argparse
import email.parser
import hashlib
import os
import pathlib
import re
import shutil
import stat
import sys
import tarfile
import urllib.error
import urllib.request
import zipfile

from packaging.tags import parse_tag
from packaging.utils import (
    canonicalize_name,
    parse_sdist_filename,
    parse_wheel_filename,
)
from packaging.version import Version

WHEEL_PLATFORMS = {
    "wheels-ubuntu-24.04-arm-ARM64": r"manylinux_(?:\d+_\d+|\d+)_aarch64",
    "wheels-macos-15": r"macosx_\d+_\d+_arm64",
    "wheels-windows-latest": r"win_amd64",
    "wheels-windows-11-arm-ARM64": r"win_arm64",
    "wheels-ubuntu-latest-x86_64-manylinux_2_27_x86_64": r"manylinux_(?:\d+_\d+|\d+)_x86_64",
    "wheels-ubuntu-latest-x86_64-manylinux_2_34_x86_64": r"manylinux_(?:\d+_\d+|\d+)_x86_64",
}


def release_version(value: str) -> Version:
    version = Version(value)
    if str(version) != value or version.local is not None or version.epoch:
        raise ValueError(f"Use a canonical public version without an epoch: {value!r}")
    return version


def prepare(version_file: pathlib.Path, mode: str, ref: str) -> str:
    value = version_file.read_text(encoding="utf-8").strip()
    version = release_version(value)
    if mode != "dry_run" and ref != "refs/heads/main":
        raise ValueError("Publish releases only from refs/heads/main")
    if mode == "release" and version.is_prerelease:
        raise ValueError("A release requires a stable version")
    if mode == "prerelease" and not version.is_prerelease:
        raise ValueError("A prerelease requires a pre-release or development version")
    return value


def check_pypi_version(version: str) -> None:
    release_version(version)
    url = f"https://pypi.org/pypi/nanovdb-editor/{version}/json"
    try:
        with urllib.request.urlopen(url, timeout=15) as response:
            status = response.status
    except urllib.error.HTTPError as error:
        status = error.code
        error.close()
    except OSError as error:
        raise ValueError(
            f"Could not check PyPI version availability: {error}"
        ) from error

    if status == 404:
        return
    if status == 200:
        raise ValueError(
            f"nanovdb-editor {version} already exists on PyPI; select a new version"
        )
    raise ValueError(f"Could not check PyPI version availability: HTTP {status}")


def metadata_field(metadata, name: str) -> str:
    values = metadata.get_all(name, [])
    if len(values) != 1:
        raise ValueError(f"Expected one {name} metadata field")
    return values[0]


def check_metadata(data: bytes, version: str) -> None:
    metadata = email.parser.BytesParser().parsebytes(data)
    if canonicalize_name(metadata_field(metadata, "Name")) != "nanovdb-editor":
        raise ValueError("Distribution metadata must name nanovdb-editor")
    if metadata_field(metadata, "Version") != version:
        raise ValueError(f"Distribution metadata must have version {version}")


def check_member(name: str, seen: set) -> None:
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or not path.parts:
        raise ValueError(f"Unsafe archive member: {name!r}")
    if name in seen:
        raise ValueError(f"Duplicate archive member: {name!r}")
    seen.add(name)


def check_wheel(path: pathlib.Path, version: str, platform: str) -> None:
    name, parsed_version, _, tags = parse_wheel_filename(path.name)
    if name != "nanovdb-editor" or str(parsed_version) != version:
        raise ValueError(f"Unexpected wheel name or version: {path.name}")
    if not all(
        tag.interpreter == "py3"
        and tag.abi == "none"
        and re.fullmatch(platform, tag.platform)
        for tag in tags
    ):
        raise ValueError(f"Unexpected wheel platform or Python tags: {path.name}")

    dist_info = f"nanovdb_editor-{version}.dist-info"
    with zipfile.ZipFile(path) as archive:
        seen = set()
        for member in archive.infolist():
            check_member(member.filename, seen)
            if stat.S_ISLNK(member.external_attr >> 16):
                raise ValueError(f"Wheel contains a symbolic link: {member.filename}")
        metadata_names = {name for name in seen if name.endswith(".dist-info/METADATA")}
        wheel_names = {name for name in seen if name.endswith(".dist-info/WHEEL")}
        if metadata_names != {f"{dist_info}/METADATA"} or wheel_names != {
            f"{dist_info}/WHEEL"
        }:
            raise ValueError(f"Expected one matching dist-info directory: {path.name}")
        check_metadata(archive.read(f"{dist_info}/METADATA"), version)
        metadata = email.parser.BytesParser().parsebytes(
            archive.read(f"{dist_info}/WHEEL")
        )
        if metadata_field(metadata, "Root-Is-Purelib").lower() != "false":
            raise ValueError(f"Expected a platform wheel: {path.name}")
        metadata_tags = set()
        for value in metadata.get_all("Tag", []):
            metadata_tags.update(parse_tag(value))
        if metadata_tags != tags:
            raise ValueError(f"WHEEL tags disagree with the filename: {path.name}")


def check_sdist(path: pathlib.Path, version: str) -> None:
    name, parsed_version = parse_sdist_filename(path.name)
    if (
        name != "nanovdb-editor"
        or str(parsed_version) != version
        or not path.name.endswith(".tar.gz")
    ):
        raise ValueError(f"Unexpected source distribution: {path.name}")
    root = path.name.removesuffix(".tar.gz")
    with tarfile.open(path, "r:gz") as archive:
        seen = set()
        for member in archive.getmembers():
            check_member(member.name, seen)
            if not (member.isfile() or member.isdir()):
                raise ValueError(
                    f"Source archive contains a link or special file: {member.name}"
                )
            if pathlib.PurePosixPath(member.name).parts[0] != root:
                raise ValueError(f"Unexpected source archive root: {member.name}")
        metadata = archive.extractfile(f"{root}/PKG-INFO")
        if metadata is None:
            raise ValueError("Source distribution has no PKG-INFO file")
        with metadata:
            check_metadata(metadata.read(), version)


def verify(
    directory: pathlib.Path, version: str, output_directory: pathlib.Path
) -> None:
    release_version(version)
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("Artifact directory must be a directory, not a symbolic link")
    if output_directory.exists() or output_directory.is_symlink():
        raise ValueError("Output directory must not exist")
    expected = set(WHEEL_PLATFORMS) | {"sdist"}
    if {path.name for path in directory.iterdir()} != expected:
        raise ValueError(
            "Expected exactly the six wheel artifacts and the sdist artifact"
        )

    files = []
    names = set()
    for artifact in sorted(directory.iterdir()):
        if artifact.is_symlink() or not artifact.is_dir():
            raise ValueError(f"Artifact must be a directory: {artifact.name}")
        entries = list(artifact.iterdir())
        if len(entries) != 1 or entries[0].is_symlink() or not entries[0].is_file():
            raise ValueError(
                f"Expected one regular distribution file in {artifact.name}"
            )
        path = entries[0]
        if path.name in names:
            raise ValueError(f"Duplicate distribution filename: {path.name}")
        names.add(path.name)
        if artifact.name == "sdist":
            check_sdist(path, version)
        else:
            check_wheel(path, version, WHEEL_PLATFORMS[artifact.name])
        files.append(path)

    output_directory.mkdir(parents=True)
    checksums = []
    for path in sorted(files, key=lambda path: path.name):
        destination = output_directory / path.name
        shutil.copyfile(path, destination)
        with destination.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksums.append(f"{digest}  {path.name}\n")
    (output_directory / "SHA256SUMS").write_text("".join(checksums), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Validate release versions and build artifacts"
    )
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument(
        "--mode", choices=("dry_run", "prerelease", "release"), required=True
    )
    prepare_parser.add_argument("--ref", required=True)
    prepare_parser.add_argument(
        "--version-file",
        type=pathlib.Path,
        default=pathlib.Path("pymodule/VERSION.txt"),
    )
    pypi_parser = commands.add_parser("check-pypi")
    pypi_parser.add_argument("--version", required=True)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--directory", type=pathlib.Path, required=True)
    verify_parser.add_argument("--version", required=True)
    verify_parser.add_argument("--output-directory", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            version = prepare(args.version_file, args.mode, args.ref)
            outputs = f"version={version}\ntag=v{version}\n"
            if "GITHUB_OUTPUT" in os.environ:
                with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
                    stream.write(outputs)
            print(outputs, end="")
        elif args.command == "check-pypi":
            check_pypi_version(args.version)
            print(f"nanovdb-editor {args.version} is not listed on PyPI")
        else:
            verify(args.directory, args.version, args.output_directory)
            print(f"Verified release {args.version}: {args.output_directory}")
    except (
        OSError,
        ValueError,
        KeyError,
        tarfile.TarError,
        zipfile.BadZipFile,
    ) as error:
        print(f"Release validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
