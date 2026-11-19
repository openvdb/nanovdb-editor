# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class FvdbVizVersionsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        scripts = self.root / "scripts"
        scripts.mkdir()
        for name in (
            "fvdb_viz_versions.sh",
            "build_fvdb_viz_test_image.sh",
            "run_fvdb_viz_integration.sh",
        ):
            shutil.copy2(REPO_ROOT / "scripts" / name, scripts / name)
        self.calls = self.root / "docker-calls.jsonl"
        docker = self.root / "docker"
        docker.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "with open(os.environ['DOCKER_CALLS'], 'a') as output:\n"
            "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "sys.exit(1 if sys.argv[1:3] == ['image', 'inspect'] else 0)\n"
        )
        docker.chmod(0o755)
        self.env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("FVDB_VIZ_")
        }
        self.env.update(
            PATH=f"{self.root}:{os.environ['PATH']}",
            DOCKER_CALLS=str(self.calls),
            FVDB_VIZ_IMAGE_CACHE_ENABLED="0",
        )

    def run_script(self, name, *args, **overrides):
        result = subprocess.run(
            ["bash", str(self.root / "scripts" / name), *args],
            cwd=self.root,
            env={**self.env, **overrides},
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    @staticmethod
    def options(call, flag):
        return dict(
            call[index + 1].split("=", 1)
            for index, arg in enumerate(call)
            if arg == flag
        )

    def check_stack(self, calls, *, nightly, torch_version, torch_index, suffix):
        build = next(call for call in calls if call[0] == "build")
        args = self.options(build, "--build-arg")
        self.assertEqual(args["TORCH_VERSION"], torch_version)
        self.assertEqual(args["TORCH_INDEX_URL"], torch_index)
        self.assertEqual(args["FVDB_CORE_NIGHTLY"], "1" if nightly else "0")
        self.assertEqual(args["FVDB_CORE_VERSION"], "0.5.1+pt211.cu128")
        if nightly:
            self.assertEqual(args["FVDB_CORE_NIGHTLY_CUDA_SUFFIX"], suffix)
        for call in calls:
            if call[0] == "run":
                env = self.options(call, "-e")
                self.assertEqual(env["FVDB_VIZ_TORCH_VERSION"], torch_version)
                self.assertEqual(env["FVDB_VIZ_TORCH_INDEX_URL"], torch_index)

    def test_stable_build_keeps_release_stack(self):
        calls = self.run_script("build_fvdb_viz_test_image.sh")
        self.check_stack(
            calls,
            nightly=False,
            torch_version="2.11.0",
            torch_index="https://download.pytorch.org/whl/cu128",
            suffix=None,
        )

    def test_nightly_build_selects_matching_torch_and_cuda(self):
        calls = self.run_script(
            "build_fvdb_viz_test_image.sh", FVDB_VIZ_CORE_NIGHTLY="1"
        )
        self.check_stack(
            calls,
            nightly=True,
            torch_version="2.13.0",
            torch_index="https://download.pytorch.org/whl/cu130",
            suffix="pt213.cu130",
        )

    def test_stable_wrapper_keeps_release_stack_in_build_and_run(self):
        calls = self.run_script("run_fvdb_viz_integration.sh")
        self.assertEqual(sum(call[0] == "run" for call in calls), 1)
        self.check_stack(
            calls,
            nightly=False,
            torch_version="2.11.0",
            torch_index="https://download.pytorch.org/whl/cu128",
            suffix=None,
        )

    def test_nightly_wrapper_uses_same_stack_in_build_and_run(self):
        calls = self.run_script("run_fvdb_viz_integration.sh", "--fvdb-nightly")
        self.assertEqual(sum(call[0] == "run" for call in calls), 1)
        self.check_stack(
            calls,
            nightly=True,
            torch_version="2.13.0",
            torch_index="https://download.pytorch.org/whl/cu130",
            suffix="pt213.cu130",
        )

    def test_build_and_wrapper_preserve_environment_overrides(self):
        overrides = {
            "FVDB_VIZ_TORCH_VERSION": "2.13.1",
            "FVDB_VIZ_TORCH_INDEX_URL": "https://torch.example/cu132",
            "FVDB_VIZ_CORE_VERSION": "0.6.0+pt213.cu132",
            "FVDB_VIZ_CORE_INDEX_URL": "https://fvdb.example/stable",
            "FVDB_VIZ_CORE_NIGHTLY_INDEX_URL": "https://fvdb.example/nightly",
            "FVDB_VIZ_CORE_NIGHTLY_CUDA_SUFFIX": "pt213.cu132",
        }
        for script in ("build_fvdb_viz_test_image.sh", "run_fvdb_viz_integration.sh"):
            for nightly in (False, True):
                with self.subTest(script=script, nightly=nightly):
                    self.calls.unlink(missing_ok=True)
                    args = (
                        ["--fvdb-nightly"]
                        if nightly and script.startswith("run_")
                        else []
                    )
                    calls = self.run_script(
                        script,
                        *args,
                        FVDB_VIZ_CORE_NIGHTLY="1" if nightly else "0",
                        **overrides,
                    )
                    build = next(call for call in calls if call[0] == "build")
                    actual = self.options(build, "--build-arg")
                    for key, expected in overrides.items():
                        argument = key.replace("FVDB_VIZ_", "")
                        if argument.startswith("CORE_"):
                            argument = "FVDB_" + argument
                        self.assertEqual(actual[argument], expected)
                    for call in calls:
                        if call[0] == "run":
                            env = self.options(call, "-e")
                            for key in (
                                "FVDB_VIZ_TORCH_VERSION",
                                "FVDB_VIZ_TORCH_INDEX_URL",
                                "FVDB_VIZ_CORE_VERSION",
                                "FVDB_VIZ_CORE_INDEX_URL",
                            ):
                                self.assertEqual(env[key], overrides[key])


if __name__ == "__main__":
    unittest.main()
