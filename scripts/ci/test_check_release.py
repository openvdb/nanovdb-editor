#!/usr/bin/env python3
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import hashlib
import io
import itertools
import os
import pathlib
import stat
import tarfile
import tempfile
import unittest
import urllib.error
import zipfile
from unittest import mock

import check_release

VERSION = "0.1.8"
PACKAGES = ("nanovdb-editor", "nanovdb-editor-dev")
PLATFORMS = {
    "wheels-ubuntu-24.04-arm-ARM64": "manylinux_2_28_aarch64.manylinux_2_17_aarch64",
    "wheels-macos-15": "macosx_15_0_arm64",
    "wheels-windows-latest": "win_amd64",
    "wheels-windows-11-arm-ARM64": "win_arm64",
    "wheels-ubuntu-latest-x86_64-manylinux_2_27_x86_64": "manylinux_2_28_x86_64",
    "wheels-ubuntu-latest-x86_64-manylinux_2_34_x86_64": "manylinux_2_34_x86_64",
}


def write_wheel(
    directory,
    platform,
    version=VERSION,
    metadata_version=None,
    name=None,
    tags=None,
    pure="false",
    extra=None,
    package="nanovdb-editor",
    dist_info_package=None,
):
    distribution = package.replace("-", "_")
    path = directory / f"{distribution}-{version}-py3-none-{platform}.whl"
    dist_info_distribution = (dist_info_package or package).replace("-", "_")
    dist_info = f"{dist_info_distribution}-{version}.dist-info"
    wheel_tags = (
        tags
        if tags is not None
        else [f"py3-none-{item}" for item in platform.split(".")]
    )
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(
            f"{dist_info}/METADATA",
            f"Metadata-Version: 2.1\nName: {name or package}\nVersion: {metadata_version or version}\n",
        )
        archive.writestr(
            f"{dist_info}/WHEEL",
            f"Wheel-Version: 1.0\nRoot-Is-Purelib: {pure}\n"
            + "".join(f"Tag: {tag}\n" for tag in wheel_tags),
        )
        if extra is not None:
            archive.writestr(extra, "payload")
    return path


def write_sdist(
    directory,
    version=VERSION,
    metadata_version=None,
    extra=None,
    package="nanovdb-editor",
    name=None,
):
    root = f"{package.replace('-', '_')}-{version}"
    path = directory / f"{root}.tar.gz"
    with tarfile.open(path, "w:gz") as archive:
        data = f"Metadata-Version: 2.1\nName: {name or package}\nVersion: {metadata_version or version}\n".encode()
        member = tarfile.TarInfo(f"{root}/PKG-INFO")
        member.size = len(data)
        archive.addfile(member, io.BytesIO(data))
        if extra is not None:
            archive.addfile(extra)
    return path


class PyPIVersionTests(unittest.TestCase):
    def test_unpublished_version_is_available(self):
        for package, version in itertools.product(
            PACKAGES, (VERSION, "0.1.8rc1", "0.1.8.dev1")
        ):
            url = f"https://pypi.org/pypi/{package}/{version}/json"
            with (
                self.subTest(package=package, version=version),
                mock.patch(
                    "check_release.urllib.request.urlopen",
                    side_effect=urllib.error.HTTPError(url, 404, "Not Found", {}, None),
                ) as urlopen,
            ):
                check_release.check_pypi_version(version, package)
                urlopen.assert_called_once_with(url, timeout=15)

    def test_published_version_is_rejected(self):
        for package in PACKAGES:
            with (
                self.subTest(package=package),
                mock.patch("check_release.urllib.request.urlopen") as urlopen,
            ):
                urlopen.return_value.__enter__.return_value.status = 200
                with self.assertRaisesRegex(ValueError, f"{package}.*already"):
                    check_release.check_pypi_version(VERSION, package)

    def test_http_failure_is_rejected(self):
        for package, status in itertools.product(PACKAGES, (403, 429, 500)):
            url = f"https://pypi.org/pypi/{package}/{VERSION}/json"
            with (
                self.subTest(package=package, status=status),
                mock.patch(
                    "check_release.urllib.request.urlopen",
                    side_effect=urllib.error.HTTPError(
                        url, status, "Request failed", {}, None
                    ),
                ),
                self.assertRaises(ValueError),
            ):
                check_release.check_pypi_version(VERSION, package)

    def test_unexpected_response_is_rejected(self):
        for package in PACKAGES:
            with (
                self.subTest(package=package),
                mock.patch("check_release.urllib.request.urlopen") as urlopen,
            ):
                urlopen.return_value.__enter__.return_value.status = 204
                with self.assertRaises(ValueError):
                    check_release.check_pypi_version(VERSION, package)

    def test_network_failure_is_rejected(self):
        for package, error in itertools.product(
            PACKAGES, (urllib.error.URLError("Connection refused"), TimeoutError())
        ):
            with (
                self.subTest(package=package, error=type(error).__name__),
                mock.patch("check_release.urllib.request.urlopen", side_effect=error),
                self.assertRaises(ValueError),
            ):
                check_release.check_pypi_version(VERSION, package)

    def test_invalid_version_is_rejected_before_request(self):
        for version in ("v0.1.8", "0.1.8+local", "1!0.1.8", "not-a-version"):
            with (
                self.subTest(version=version),
                mock.patch("check_release.urllib.request.urlopen") as urlopen,
            ):
                with self.assertRaises(ValueError):
                    check_release.check_pypi_version(version)
                urlopen.assert_not_called()


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.output = self.root / "dist"

    def prepare(self, version, mode="dry_run", ref="refs/heads/main"):
        path = self.root / "VERSION.txt"
        path.write_text(version + "\n", encoding="utf-8")
        return check_release.prepare(path, mode, ref)

    def populate(self, package="nanovdb-editor"):
        for artifact, platform in PLATFORMS.items():
            directory = self.artifacts / artifact
            directory.mkdir()
            write_wheel(directory, platform, package=package)
        directory = self.artifacts / "sdist"
        directory.mkdir()
        write_sdist(directory, package=package)

    def verify(self, package="nanovdb-editor"):
        check_release.verify(self.artifacts, VERSION, self.output, package)

    def replace_wheel(self, artifact="wheels-windows-latest", **kwargs):
        directory = self.artifacts / artifact
        for path in directory.iterdir():
            path.unlink()
        return write_wheel(
            directory, kwargs.pop("platform", PLATFORMS[artifact]), **kwargs
        )

    def test_release_modes(self):
        self.assertEqual(self.prepare(VERSION, "release"), VERSION)
        for version in ("0.1.8rc1", "0.1.8.dev1"):
            self.assertEqual(self.prepare(version, "prerelease"), version)
            with self.assertRaisesRegex(ValueError, "stable"):
                self.prepare(version, "release")
        with self.assertRaisesRegex(ValueError, "pre-release"):
            self.prepare(VERSION, "prerelease")

    def test_publication_requires_main(self):
        for mode, version in (("release", VERSION), ("prerelease", "0.1.8rc1")):
            for ref in ("refs/heads/release/test", "refs/tags/v0.1.8", "main"):
                with (
                    self.subTest(mode=mode, ref=ref),
                    self.assertRaisesRegex(ValueError, "main"),
                ):
                    self.prepare(version, mode, ref)
        self.assertEqual(self.prepare(VERSION, ref="refs/heads/topic"), VERSION)

    def test_dev_publication_requires_a_branch(self):
        for version, ref in itertools.product(
            (VERSION, "0.1.8rc1", "0.1.8.dev1"),
            ("refs/heads/main", "refs/heads/release/test"),
        ):
            with self.subTest(version=version, ref=ref):
                self.assertEqual(self.prepare(version, "dev", ref), version)
        for ref in ("refs/tags/v0.1.8", "refs/pull/225/merge", "main"):
            with self.subTest(ref=ref), self.assertRaisesRegex(ValueError, "branch"):
                self.prepare(VERSION, "dev", ref)

    def test_dev_dry_run_accepts_branches_and_tags(self):
        for ref in ("refs/heads/topic", "refs/tags/v0.1.8"):
            with self.subTest(ref=ref):
                self.assertEqual(self.prepare(VERSION, "dry_run_dev", ref), VERSION)

    def test_reject_noncanonical_and_private_versions(self):
        for version in (
            "v0.1.8",
            "0.1.8-rc1",
            "0.1.8+local",
            "1!0.1.8",
            "0!0.1.8",
            "0.01.8",
            "not-a-version",
            "0.1.8\ntag=other",
        ):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.prepare(version)

    def test_complete_artifacts_generate_checksums(self):
        for package in PACKAGES:
            with self.subTest(package=package):
                self.artifacts = self.root / package
                self.artifacts.mkdir()
                self.output = self.root / f"{package}-dist"
                self.populate(package)
                self.verify(package)
                lines = (self.output / "SHA256SUMS").read_text().splitlines()
                self.assertEqual(len(lines), 7)
                self.assertEqual(len(list(self.output.iterdir())), 8)
                for line in lines:
                    digest, name = line.split("  ")
                    self.assertTrue(name.startswith(package.replace("-", "_") + "-"))
                    self.assertEqual(
                        digest,
                        hashlib.sha256((self.output / name).read_bytes()).hexdigest(),
                    )

    def test_existing_output_is_preserved(self):
        self.populate()
        self.output.mkdir()
        marker = self.output / "marker"
        marker.write_text("keep")
        with self.assertRaisesRegex(ValueError, "must not exist"):
            self.verify()
        self.assertEqual(marker.read_text(), "keep")

    def test_missing_or_extra_artifact(self):
        self.populate()
        directory = self.artifacts / "sdist"
        directory.rename(self.artifacts / "unexpected")
        with self.assertRaisesRegex(ValueError, "exactly"):
            self.verify()
        self.assertFalse(self.output.exists())

    def test_extra_file(self):
        self.populate()
        (self.artifacts / "sdist" / "unexpected.txt").write_text("extra")
        with self.assertRaisesRegex(ValueError, "one regular"):
            self.verify()

    def test_symlink_distribution(self):
        self.populate()
        path = next((self.artifacts / "sdist").iterdir())
        target = self.root / path.name
        path.rename(target)
        path.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "one regular"):
            self.verify()

    def test_symlink_artifact(self):
        self.populate()
        directory = self.artifacts / "sdist"
        directory.rename(self.root / "sdist")
        directory.symlink_to(self.root / "sdist", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "must be a directory"):
            self.verify()

    def test_duplicate_wheel_filenames(self):
        self.populate()
        self.replace_wheel(
            "wheels-ubuntu-latest-x86_64-manylinux_2_34_x86_64",
            platform="manylinux_2_28_x86_64",
        )
        with self.assertRaisesRegex(ValueError, "Duplicate distribution"):
            self.verify()

    def test_wrong_platform(self):
        self.populate()
        self.replace_wheel(platform="manylinux_2_28_aarch64")
        with self.assertRaisesRegex(ValueError, "platform or Python tags"):
            self.verify()

    def test_wrong_filename_version(self):
        self.populate()
        self.replace_wheel(version="0.1.9")
        with self.assertRaisesRegex(ValueError, "wheel name or version"):
            self.verify()

    def test_package_filename_must_match_selected_package(self):
        for actual, expected in (PACKAGES, PACKAGES[::-1]):
            with self.subTest(actual=actual, expected=expected):
                wheel = write_wheel(self.root, "win_amd64", package=actual)
                with self.assertRaisesRegex(ValueError, "wheel name or version"):
                    check_release.check_wheel(wheel, VERSION, "win_amd64", expected)
                sdist = write_sdist(self.root, package=actual)
                with self.assertRaisesRegex(ValueError, "source distribution"):
                    check_release.check_sdist(sdist, VERSION, expected)

    def test_dist_info_must_match_selected_package(self):
        for package, other in (PACKAGES, PACKAGES[::-1]):
            with self.subTest(package=package):
                wheel = write_wheel(
                    self.root, "win_amd64", package=package, dist_info_package=other
                )
                with self.assertRaisesRegex(ValueError, "dist-info"):
                    check_release.check_wheel(wheel, VERSION, "win_amd64", package)

    def test_metadata_must_match_selected_package(self):
        for package, other in (PACKAGES, PACKAGES[::-1]):
            with self.subTest(package=package):
                wheel = write_wheel(self.root, "win_amd64", package=package, name=other)
                with self.assertRaisesRegex(ValueError, "metadata"):
                    check_release.check_wheel(wheel, VERSION, "win_amd64", package)
                sdist = write_sdist(self.root, package=package, name=other)
                with self.assertRaisesRegex(ValueError, "metadata"):
                    check_release.check_sdist(sdist, VERSION, package)

    def test_python_specific_wheel(self):
        self.populate()
        path = self.replace_wheel()
        path.rename(path.with_name(path.name.replace("py3-none", "cp311-cp311")))
        with self.assertRaisesRegex(ValueError, "platform or Python tags"):
            self.verify()

    def test_missing_wheel_metadata(self):
        self.populate()
        path = self.replace_wheel()
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("unrelated.txt", "payload")
        with self.assertRaisesRegex(ValueError, "dist-info"):
            self.verify()

    def test_metadata_version_and_name(self):
        self.populate()
        for kwargs in ({"metadata_version": "0.1.9"}, {"name": "nanovdb-editor-dev"}):
            self.replace_wheel(**kwargs)
            with (
                self.subTest(kwargs=kwargs),
                self.assertRaisesRegex(ValueError, "metadata"),
            ):
                self.verify()

    def test_wheel_tag_consistency(self):
        self.populate()
        self.replace_wheel(tags=["cp311-cp311-win_amd64"])
        with self.assertRaisesRegex(ValueError, "WHEEL tags"):
            self.verify()

    def test_platform_wheel_must_not_be_pure(self):
        self.populate()
        self.replace_wheel(pure="true")
        with self.assertRaisesRegex(ValueError, "platform wheel"):
            self.verify()

    def test_unsafe_wheel_member(self):
        self.populate()
        self.replace_wheel(extra="../outside")
        with self.assertRaisesRegex(ValueError, "Unsafe archive"):
            self.verify()
        self.assertFalse((self.root / "outside").exists())

    def test_wheel_symlink(self):
        self.populate()
        link = zipfile.ZipInfo("nanovdb_editor/link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.replace_wheel(extra=link)
        with self.assertRaisesRegex(ValueError, "symbolic link"):
            self.verify()

    def test_sdist_version(self):
        self.populate()
        write_sdist(self.artifacts / "sdist", metadata_version="0.1.9")
        with self.assertRaisesRegex(ValueError, "metadata"):
            self.verify()

    def test_sdist_link(self):
        self.populate()
        link = tarfile.TarInfo(f"nanovdb_editor-{VERSION}/link")
        link.type = tarfile.SYMTYPE
        link.linkname = "../../outside"
        write_sdist(self.artifacts / "sdist", extra=link)
        with self.assertRaisesRegex(ValueError, "link or special file"):
            self.verify()

    def test_sdist_path_traversal(self):
        self.populate()
        write_sdist(self.artifacts / "sdist", extra=tarfile.TarInfo("../outside"))
        with self.assertRaisesRegex(ValueError, "Unsafe archive"):
            self.verify()
        self.assertFalse((self.root / "outside").exists())

    def test_prepare_cli_writes_outputs(self):
        version_file = self.root / "VERSION.txt"
        output_file = self.root / "github-output"
        for mode, package, version, ref in (
            ("dry_run", "nanovdb-editor", VERSION, "refs/heads/topic"),
            ("release", "nanovdb-editor", VERSION, "refs/heads/main"),
            ("prerelease", "nanovdb-editor", "0.1.8rc1", "refs/heads/main"),
            ("dev", "nanovdb-editor-dev", VERSION, "refs/heads/topic"),
            ("dry_run_dev", "nanovdb-editor-dev", VERSION, "refs/tags/v0.1.8"),
        ):
            version_file.write_text(version + "\n", encoding="utf-8")
            output_file.write_text("existing=keep\n", encoding="utf-8")
            arguments = [
                "check_release.py",
                "prepare",
                "--version-file",
                str(version_file),
                "--mode",
                mode,
                "--ref",
                ref,
            ]
            with (
                self.subTest(mode=mode),
                mock.patch("sys.argv", arguments),
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(output_file)}),
                mock.patch("sys.stdout", new_callable=io.StringIO) as stdout,
            ):
                check_release.main()
                expected = f"version={version}\ntag=v{version}\npackage={package}\n"
                self.assertEqual(stdout.getvalue(), expected)
                self.assertEqual(output_file.read_text(), "existing=keep\n" + expected)

    def test_pypi_cli_selects_package(self):
        for package, options in (
            ("nanovdb-editor", []),
            ("nanovdb-editor-dev", ["--package", "nanovdb-editor-dev"]),
        ):
            arguments = [
                "check_release.py",
                "check-pypi",
                "--version",
                VERSION,
                *options,
            ]
            with (
                self.subTest(package=package),
                mock.patch("sys.argv", arguments),
                mock.patch("check_release.check_pypi_version") as check,
                mock.patch("sys.stdout", new_callable=io.StringIO) as stdout,
            ):
                check_release.main()
                check.assert_called_once_with(VERSION, package)
                self.assertEqual(
                    stdout.getvalue(), f"{package} {VERSION} is not listed on PyPI\n"
                )

    def test_verify_cli_selects_package(self):
        for package, options in (
            ("nanovdb-editor", []),
            ("nanovdb-editor-dev", ["--package", "nanovdb-editor-dev"]),
        ):
            arguments = [
                "check_release.py",
                "verify",
                "--version",
                VERSION,
                "--directory",
                str(self.artifacts),
                "--output-directory",
                str(self.output),
                *options,
            ]
            with (
                self.subTest(package=package),
                mock.patch("sys.argv", arguments),
                mock.patch("check_release.verify") as verify,
                mock.patch("sys.stdout", new_callable=io.StringIO) as stdout,
            ):
                check_release.main()
                verify.assert_called_once_with(
                    self.artifacts, VERSION, self.output, package
                )
                self.assertEqual(
                    stdout.getvalue(),
                    f"Verified {package} {VERSION}: {self.output}\n",
                )

    def test_cli_rejects_unknown_mode_and_package(self):
        for arguments in (
            ["prepare", "--mode", "unknown", "--ref", "refs/heads/main"],
            ["check-pypi", "--version", VERSION, "--package", "unrelated"],
            [
                "verify",
                "--version",
                VERSION,
                "--directory",
                str(self.artifacts),
                "--output-directory",
                str(self.output),
                "--package",
                "unrelated",
            ],
        ):
            with (
                self.subTest(command=arguments[0]),
                mock.patch("sys.argv", ["check_release.py", *arguments]),
                mock.patch("sys.stderr", new_callable=io.StringIO) as stderr,
            ):
                with self.assertRaises(SystemExit) as error:
                    check_release.main()
                self.assertEqual(error.exception.code, 2)
                self.assertIn("invalid choice", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
