# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest


RUNS_ON_FVDB_VIZ_ENV = os.environ.get("FVDB_VIZ_TESTS") == "1"

pytestmark = pytest.mark.skipif(
    not RUNS_ON_FVDB_VIZ_ENV,
    reason=("Set FVDB_VIZ_TESTS=1 to enable fvdb.viz integration checks " "(runs in CI Docker)."),
)

TORCH_VERSION = os.environ.get("FVDB_VIZ_TORCH_VERSION", "2.8.0")
TORCH_INDEX_URL = os.environ.get(
    "FVDB_VIZ_TORCH_INDEX_URL",
    "https://download.pytorch.org/whl/cu128",
)
FVDB_CORE_VERSION = os.environ.get(
    "FVDB_VIZ_CORE_VERSION",
    "0.3.0+pt28.cu128",
)
FVDB_CORE_INDEX_URL = os.environ.get(
    "FVDB_VIZ_CORE_INDEX_URL",
    "https://d36m13axqqhiit.cloudfront.net/simple",
)
SCRIPT_TIMEOUT = int(os.environ.get("FVDB_VIZ_SCRIPT_TIMEOUT", "300"))

_NOTEBOOK_STYLE_SCRIPT = textwrap.dedent(
    """
    import sys
    import webbrowser

    import torch

    webbrowser.open_new_tab = lambda url: None

    import fvdb.viz

    try:
        fvdb.viz.init(port=8123, verbose=True)
    except Exception as exc:
        print(
            f\"FVDB_VIZ_INIT_FAILED: {exc}\",
            file=sys.stderr,
        )
        raise

    scene = fvdb.viz.get_scene(\"NotebookScene\")

    points = torch.tensor(
        [
            [0.0, 0.0, 0.0],
            [0.5, 0.2, 0.1],
            [0.1, 0.6, 0.9],
        ],
        dtype=torch.float32,
    )
    colors = torch.tensor(
        [
            [1.0, 0.2, 0.3],
            [0.2, 1.0, 0.3],
            [0.2, 0.3, 1.0],
        ],
        dtype=torch.float32,
    )
    scene.add_point_cloud(
        \"sample_points\",
        points,
        colors,
        point_size=3.0,
    )

    camera_to_world = torch.eye(4).unsqueeze(0)
    projection = torch.eye(3).unsqueeze(0)
    scene.add_cameras(
        \"sample_cameras\",
        camera_to_world,
        projection,
    )
    scene.set_camera_lookat(
        torch.tensor([2.0, 2.0, 2.0]),
        torch.tensor([0.0, 0.0, 0.0]),
    )

    fvdb.viz.show()
    """
)


def _venv_bin_path(venv_path: Path, binary: str) -> Path:
    scripts_dir = "Scripts" if os.name == "nt" else "bin"
    return venv_path / scripts_dir / binary


def _run(cmd: list[str], env: dict[str, str], **kwargs):
    result = subprocess.run(
        cmd,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        **kwargs,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "Command "
            f"{cmd} failed with code {result.returncode}\\n"
            f"STDOUT:\\n{result.stdout}\\n"
            f"STDERR:\\n{result.stderr}"
        )
    return result


@pytest.fixture(name="fvdb_viz_env", scope="module")
def _fvdb_viz_env(tmp_path_factory):
    venv_root = tmp_path_factory.mktemp("fvdb_viz_env")
    subprocess.run([sys.executable, "-m", "venv", str(venv_root)], check=True)

    env = os.environ.copy()
    env.setdefault("PIP_DISABLE_PIP_VERSION_CHECK", "1")
    env.setdefault("PYTHONNOUSERSITE", "1")

    python_exe = _venv_bin_path(venv_root, "python")
    pip_exe = _venv_bin_path(venv_root, "pip")

    _run([str(pip_exe), "install", "--upgrade", "pip"], env=env)
    _run(
        [
            str(pip_exe),
            "install",
            f"torch=={TORCH_VERSION}",
            "--extra-index-url",
            TORCH_INDEX_URL,
        ],
        env=env,
    )
    _run(
        [
            str(pip_exe),
            "install",
            f"fvdb-core=={FVDB_CORE_VERSION}",
            "--extra-index-url",
            FVDB_CORE_INDEX_URL,
        ],
        env=env,
    )

    return {"python": python_exe, "pip": pip_exe, "env": env}


def _install_nanovdb_distribution(env_ctx: dict, dist_name: str):
    pip_exe = env_ctx["pip"]
    env = env_ctx["env"]
    subprocess.run(
        [
            str(pip_exe),
            "uninstall",
            "-y",
            "nanovdb-editor",
            "nanovdb-editor-dev",
        ],
        env=env,
        check=False,
    )
    _run([str(pip_exe), "install", dist_name], env=env)


def _run_notebook_script(env_ctx: dict):
    python_exe = env_ctx["python"]
    env = env_ctx["env"]
    proc = subprocess.run(
        [str(python_exe), "-c", _NOTEBOOK_STYLE_SCRIPT],
        env=env,
        capture_output=True,
        text=True,
        timeout=SCRIPT_TIMEOUT,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError("Viewer test failed:\\n" f"STDOUT:\\n{proc.stdout}\\nSTDERR:\\n{proc.stderr}")


@pytest.mark.slow
def test_fvdb_viz_with_release_package(fvdb_viz_env):
    """
    Ensure the fvdb.viz notebook workflow works with the latest pip release of
    nanovdb-editor.
    """
    _install_nanovdb_distribution(fvdb_viz_env, "nanovdb-editor")
    _run_notebook_script(fvdb_viz_env)


@pytest.mark.slow
def test_fvdb_viz_with_dev_package(fvdb_viz_env):
    """
    The development-only nanovdb-editor-dev build is expected to regress viewer
    support; fail loudly if that changes.
    """
    _install_nanovdb_distribution(fvdb_viz_env, "nanovdb-editor-dev")
    _run_notebook_script(fvdb_viz_env)
