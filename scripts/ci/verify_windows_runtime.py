#!/usr/bin/env python3
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import pathlib
import site
import sys
import zipfile


def windows_h264_enabled() -> bool:
    env_value = os.environ.get("NANOVDB_EDITOR_WINDOWS_H264")
    if env_value is not None:
        return env_value == "1"

    for source in (
        os.environ.get("CMAKE_ARGS", ""),
        os.environ.get("CIBW_ENVIRONMENT", ""),
    ):
        if "-DNANOVDB_EDITOR_USE_H264=ON" in source:
            return True

    return False


def has_dll(entries, prefix: str) -> bool:
    prefix = prefix.lower()
    return any(
        entry.lower().startswith(prefix) and entry.lower().endswith(".dll")
        for entry in entries
    )


def verify_wheel(wheel_dir: pathlib.Path) -> None:
    wheels = sorted(wheel_dir.glob("nanovdb_editor*.whl"))
    if not wheels:
        print(f"No nanovdb_editor wheel found in {wheel_dir}/", file=sys.stderr)
        raise SystemExit(1)

    wheel = wheels[0]
    print(f"Inspecting wheel: {wheel}")
    with zipfile.ZipFile(wheel) as zf:
        names = zf.namelist()

    lower_names = {name.lower() for name in names}
    bundled_libs = sorted(
        name for name in names if name.startswith("nanovdb_editor/lib/")
    )

    print("Bundled runtime libs:")
    for name in bundled_libs:
        print(f" - {name}")

    missing = []
    if "nanovdb_editor/lib/vulkan-1.dll" not in lower_names:
        missing.append("vulkan-1.dll")
    if windows_h264_enabled() and not has_dll(
        bundled_libs, "nanovdb_editor/lib/openh264"
    ):
        missing.append("openh264*.dll")

    if missing:
        print(
            "Windows wheel is missing required runtime libraries: "
            + ", ".join(missing),
            file=sys.stderr,
        )
        raise SystemExit(1)


def find_installed_package_dir() -> pathlib.Path:
    candidates = []
    for base in list(getattr(site, "getsitepackages", lambda: [])()):
        candidates.append(pathlib.Path(base) / "nanovdb_editor")
    user_site = getattr(site, "getusersitepackages", lambda: None)()
    if user_site:
        candidates.append(pathlib.Path(user_site) / "nanovdb_editor")

    pkg_dir = next((path for path in candidates if path.is_dir()), None)
    if pkg_dir is None:
        print(
            "Installed nanovdb_editor package directory was not found",
            file=sys.stderr,
        )
        raise SystemExit(1)
    return pkg_dir


def verify_installed(compiler_smoke_test: bool) -> None:
    pkg_dir = find_installed_package_dir()
    lib_dir = pkg_dir / "lib"
    print(f"Installed package dir: {pkg_dir}")
    print(f"Installed lib dir: {lib_dir}")
    if not lib_dir.is_dir():
        print("Installed nanovdb_editor/lib directory was not found", file=sys.stderr)
        raise SystemExit(1)

    libs = sorted(path.name for path in lib_dir.iterdir())
    lower_libs = {name.lower() for name in libs}

    print("Installed runtime libs:")
    for name in libs:
        print(f" - {name}")

    missing = []
    if "vulkan-1.dll" not in lower_libs:
        missing.append("vulkan-1.dll")
    if windows_h264_enabled() and not has_dll(libs, "openh264"):
        missing.append("openh264*.dll")

    if missing:
        print(
            "Installed wheel is missing required runtime libraries: "
            + ", ".join(missing),
            file=sys.stderr,
        )
        raise SystemExit(1)

    if compiler_smoke_test:
        import nanovdb_editor
        from nanovdb_editor import Compiler

        print(f"Imported nanovdb_editor from: {nanovdb_editor.__file__}")
        compiler = Compiler()
        compiler.create_instance()
        print("Compiler smoke test passed")
        compiler._instance = None
        compiler._compiler = None


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    wheel_parser = subparsers.add_parser("wheel")
    wheel_parser.add_argument("--dir", default="dist")

    installed_parser = subparsers.add_parser("installed")
    installed_parser.add_argument(
        "--compiler-smoke-test",
        action="store_true",
    )

    args = parser.parse_args()

    if args.command == "wheel":
        verify_wheel(pathlib.Path(args.dir))
    else:
        verify_installed(args.compiler_smoke_test)


if __name__ == "__main__":
    main()
