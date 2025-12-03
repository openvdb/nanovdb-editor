# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
VERSIONS_FILE = REPO_ROOT / "scripts" / "fvdb_viz_versions.sh"


@lru_cache(maxsize=1)
def _load_fvdb_defaults() -> dict[str, str]:
    defaults: dict[str, str] = {}
    try:
        content = VERSIONS_FILE.read_text()
    except FileNotFoundError:
        return defaults

    for line in content.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, raw_value = line.split("=", 1)
        if not key.endswith("_DEFAULT"):
            continue
        value = raw_value.strip()
        if value.startswith(("'", '"')) and value.endswith(("'", '"')):
            value = value[1:-1]
        defaults[key] = value

    return defaults


def _default_version(var_name: str, fallback: str) -> str:
    env_value = os.environ.get(var_name)
    if env_value:
        return env_value
    defaults = _load_fvdb_defaults()
    return defaults.get(f"{var_name}_DEFAULT", fallback)


RUNS_ON_FVDB_VIZ_ENV = os.environ.get("FVDB_VIZ_TESTS") == "1"
LOCAL_DIST_SPEC = os.environ.get("FVDB_VIZ_LOCAL_DIST")

pytestmark = pytest.mark.skipif(
    not RUNS_ON_FVDB_VIZ_ENV,
    reason=("Set FVDB_VIZ_TESTS=1 to enable fvdb.viz integration checks " "(runs in CI Docker)."),
)

TORCH_VERSION = _default_version("FVDB_VIZ_TORCH_VERSION", "2.8.0")
TORCH_INDEX_URL = _default_version(
    "FVDB_VIZ_TORCH_INDEX_URL",
    "https://download.pytorch.org/whl/cu128",
)
FVDB_CORE_VERSION = _default_version(
    "FVDB_VIZ_CORE_VERSION",
    "0.3.0+pt28.cu128",
)
FVDB_CORE_INDEX_URL = _default_version(
    "FVDB_VIZ_CORE_INDEX_URL",
    "https://d36m13axqqhiit.cloudfront.net/simple",
)
UPSTREAM_TEST_PATH = Path(__file__).with_name("fvdb_viz_test.py")


def _run(cmd: list[str], env: dict[str, str], **kwargs):
    try:
        subprocess.run(
            cmd,
            env=env,
            check=True,
            **kwargs,
        )
    except subprocess.CalledProcessError as exc:  # pragma: no cover
        raise RuntimeError(f"Command {cmd} failed with code {exc.returncode}") from exc


def _venv_bin_path(venv_path: Path, binary: str) -> Path:
    scripts_dir = "Scripts" if os.name == "nt" else "bin"
    return venv_path / scripts_dir / binary


@pytest.fixture(name="fvdb_viz_env", scope="module")
def _fvdb_viz_env(tmp_path_factory):
    skip_base_setup = os.environ.get("FVDB_VIZ_SKIP_BASE_INSTALL") == "1"

    env = os.environ.copy()
    env.setdefault("PIP_DISABLE_PIP_VERSION_CHECK", "1")
    env.setdefault("PYTHONNOUSERSITE", "1")
    env.setdefault("PYTHONUNBUFFERED", "1")

    if skip_base_setup:
        python_exe = sys.executable
        pip_cmd = [sys.executable, "-m", "pip"]
        env.setdefault("PIP_BREAK_SYSTEM_PACKAGES", "1")
    else:
        venv_root = tmp_path_factory.mktemp("fvdb_viz_env")
        subprocess.run(
            [sys.executable, "-m", "venv", str(venv_root)],
            check=True,
        )

        python_exe = _venv_bin_path(venv_root, "python")
        pip_exe = _venv_bin_path(venv_root, "pip")
        env["VIRTUAL_ENV"] = str(venv_root)
        env["PATH"] = f"{pip_exe.parent}:{env.get('PATH', '')}"
        pip_cmd = [str(pip_exe)]

    if not skip_base_setup:
        _run(pip_cmd + ["install", "--upgrade", "pip"], env=env)
        _run(pip_cmd + ["install", "pytest", "numpy"], env=env)
        torch_args = [
            "install",
            f"torch=={TORCH_VERSION}",
        ]
        if TORCH_INDEX_URL:
            torch_args += ["--extra-index-url", TORCH_INDEX_URL]
        _run(pip_cmd + torch_args, env=env)
        _run(
            pip_cmd
            + [
                "install",
                f"fvdb-core=={FVDB_CORE_VERSION}",
                "--extra-index-url",
                FVDB_CORE_INDEX_URL,
            ],
            env=env,
        )

    return {
        "env": env,
        "pip_cmd": pip_cmd,
        "python": python_exe,
    }


def _install_nanovdb_distribution(env_ctx: dict, dist_name: str):
    pip_cmd = env_ctx["pip_cmd"]
    env = env_ctx["env"]
    subprocess.run(
        [
            *pip_cmd,
            "uninstall",
            "-y",
            "nanovdb-editor",
            "nanovdb-editor-dev",
        ],
        env=env,
        check=False,
    )
    _run(pip_cmd + ["install", dist_name], env=env)
    _log_nanovdb_version(env_ctx)


def _log_nanovdb_version(env_ctx: dict):
    python_exe = env_ctx["python"]
    env = env_ctx["env"]
    _run(
        [
            str(python_exe),
            "-c",
            (
                "import importlib; "
                "mod = importlib.import_module('nanovdb_editor'); "
                "print("
                "'nanovdb_editor version:', "
                "getattr(mod, '__version__', 'unknown')"
                ")"
            ),
        ],
        env=env,
    )


def _run_upstream_viz_suite(env_ctx: dict):
    python_exe = env_ctx["python"]
    env = env_ctx["env"]
    cmd = [
        str(python_exe),
        "-m",
        "pytest",
        str(UPSTREAM_TEST_PATH),
        "-vv",
        "-s",
        "--maxfail=1",
        "--full-trace",
        "-rA",
    ]
    subprocess.run(cmd, env=env, check=True)


@pytest.mark.slow
def test_fvdb_viz_with_release_package(fvdb_viz_env):
    """
    Ensure the fvdb.viz notebook workflow works with the latest pip release of
    nanovdb-editor.
    """
    _install_nanovdb_distribution(fvdb_viz_env, "nanovdb-editor")
    _run_upstream_viz_suite(fvdb_viz_env)


@pytest.mark.slow
def test_fvdb_viz_with_dev_package(fvdb_viz_env):
    """
    Ensure the fvdb.viz notebook workflow works with the latest pip release of
    nanovdb-editor-dev.
    """
    _install_nanovdb_distribution(fvdb_viz_env, "nanovdb-editor-dev")
    _run_upstream_viz_suite(fvdb_viz_env)


@pytest.mark.slow
@pytest.mark.skipif(
    not LOCAL_DIST_SPEC,
    reason=("Set FVDB_VIZ_LOCAL_DIST to the local wheel or source path to test it."),
)
def test_fvdb_viz_with_local_package(fvdb_viz_env):
    """
    Run the fvdb.viz workflow against a locally built nanovdb-editor artifact.
    The FVDB_VIZ_LOCAL_DIST environment variable should point to a wheel
    file (dist/*.whl) or a source directory (for example the repo root for
    `pip install .`).
    """
    local_spec = Path(LOCAL_DIST_SPEC).expanduser()
    if not local_spec.is_absolute():
        local_spec = (Path.cwd() / local_spec).resolve()
    if not local_spec.exists():
        raise AssertionError(f"FVDB_VIZ_LOCAL_DIST target does not exist: {local_spec}")

    _install_nanovdb_distribution(fvdb_viz_env, str(local_spec))
    _run_upstream_viz_suite(fvdb_viz_env)
