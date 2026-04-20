# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import subprocess
import sys
import urllib.error
import urllib.request
from functools import lru_cache
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
VERSIONS_FILE = REPO_ROOT / "scripts" / "fvdb_viz_versions.sh"
UPSTREAM_TEST_RELATIVE_PATH = "tests/unit/test_viz.py"


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


def _default_version(var_name: str, fallback: str | None = None) -> str:
    env_value = os.environ.get(var_name)
    if env_value:
        return env_value
    defaults = _load_fvdb_defaults()
    default_value = defaults.get(f"{var_name}_DEFAULT")
    if default_value is not None:
        return default_value
    if fallback is not None:
        return fallback
    raise RuntimeError(f"Missing required default for {var_name}")


RUNS_ON_FVDB_VIZ_ENV = os.environ.get("FVDB_VIZ_TESTS") == "1"
LOCAL_DIST_SPEC = os.environ.get("FVDB_VIZ_LOCAL_DIST")
UPSTREAM_TEST_REF = os.environ.get("FVDB_VIZ_UPSTREAM_TEST_REF", "main")

pytestmark = pytest.mark.skipif(
    not RUNS_ON_FVDB_VIZ_ENV,
    reason=("Set FVDB_VIZ_TESTS=1 to enable fvdb.viz integration checks"),
)

TORCH_VERSION = _default_version("FVDB_VIZ_TORCH_VERSION", "2.10.0")
TORCH_INDEX_URL = _default_version(
    "FVDB_VIZ_TORCH_INDEX_URL",
    "https://download.pytorch.org/whl/cu128",
)
FVDB_CORE_VERSION = _default_version(
    "FVDB_VIZ_CORE_VERSION",
)
FVDB_CORE_INDEX_URL = _default_version(
    "FVDB_VIZ_CORE_INDEX_URL",
    "https://d36m13axqqhiit.cloudfront.net/simple",
)


def _fvdb_core_release_tag() -> str:
    return f"v{FVDB_CORE_VERSION.split('+', 1)[0]}"


def _upstream_test_urls() -> list[str]:
    release_tag = _fvdb_core_release_tag()
    release_url = "https://raw.githubusercontent.com/openvdb/fvdb-core/" f"{release_tag}/{UPSTREAM_TEST_RELATIVE_PATH}"
    main_url = "https://raw.githubusercontent.com/openvdb/fvdb-core/main/" f"{UPSTREAM_TEST_RELATIVE_PATH}"

    if UPSTREAM_TEST_REF == "main":
        return [main_url]
    if UPSTREAM_TEST_REF == "release":
        return [release_url]
    return [release_url, main_url]


def _resolve_upstream_test_path(work_dir: Path) -> Path:
    target_path = work_dir / "fvdb_upstream_test_viz.py"
    last_error = None

    for url in _upstream_test_urls():
        request = urllib.request.Request(
            url,
            headers={"User-Agent": "nanovdb-editor-fvdb-viz-integration"},
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                target_path.write_bytes(response.read())
            return target_path
        except (urllib.error.URLError, TimeoutError) as exc:
            last_error = exc

    if last_error is None:
        raise RuntimeError("Could not resolve an fvdb upstream viz test URL")

    raise RuntimeError("Failed to fetch upstream fvdb viz test from any known URL") from last_error


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


def _resolved_local_path(path_str: str) -> Path:
    path = Path(path_str).expanduser()
    if not path.is_absolute():
        path = (Path.cwd() / path).resolve()
    return path


def _venv_bin_path(venv_path: Path, binary: str) -> Path:
    scripts_dir = "Scripts" if os.name == "nt" else "bin"
    return venv_path / scripts_dir / binary


@pytest.fixture(name="fvdb_viz_env", scope="module")
def _fvdb_viz_env(tmp_path_factory):
    skip_base_setup = os.environ.get("FVDB_VIZ_SKIP_BASE_INSTALL") == "1"
    work_dir = tmp_path_factory.mktemp("fvdb_viz_suite")

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
                f"fvdb-core[viewer]=={FVDB_CORE_VERSION}",
                "--extra-index-url",
                FVDB_CORE_INDEX_URL,
            ],
            env=env,
        )

    return {
        "env": env,
        "pip_cmd": pip_cmd,
        "python": python_exe,
        "upstream_test_path": _resolve_upstream_test_path(work_dir),
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


def _assert_viz_server_initializes(env_ctx: dict):
    python_exe = env_ctx["python"]
    env = env_ctx["env"]
    try:
        subprocess.run(
            [
                str(python_exe),
                "-c",
                (
                    "import os; "
                    "import fvdb; "
                    "print("
                    "'VK_ICD_FILENAMES:', "
                    "os.environ.get('VK_ICD_FILENAMES', '<unset>')"
                    "); "
                    "print("
                    "'VK_DRIVER_FILES:', "
                    "os.environ.get('VK_DRIVER_FILES', '<unset>')"
                    "); "
                    "fvdb.viz.init("
                    "ip_address='127.0.0.1', port=8080, verbose=False"
                    "); "
                    "print('fvdb.viz init preflight: ok')"
                ),
            ],
            env=env,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        raise AssertionError(
            "fvdb.viz viewer initialization failed during preflight. "
            "Treating this as a hard failure instead of allowing the upstream "
            "suite to skip."
        ) from exc


def _apply_fvdb_camera_fov_compat():
    import math

    import fvdb.viz._scene as scene_module

    scene_cls = scene_module.Scene
    if getattr(scene_cls, "_nve_camera_fov_compat", False):
        return

    from fvdb.viz._viewer_server import _get_viewer_server_cpp

    def _fov_get(self):
        server = _get_viewer_server_cpp()
        try:
            return server.camera_fov(self._name)
        except (AttributeError, TypeError):
            return getattr(self, "_nve_camera_fov", math.pi / 4.0)

    def _fov_set(self, fov_radians):
        if not math.isfinite(fov_radians):
            raise ValueError(f"FOV must be a finite value, got {fov_radians}")
        if fov_radians <= 0.0 or fov_radians >= math.pi:
            raise ValueError(f"FOV must be between 0 and pi radians, got {fov_radians}")
        server = _get_viewer_server_cpp()
        try:
            server.set_camera_fov(self._name, fov_radians)
        except (AttributeError, TypeError):
            self._nve_camera_fov = fov_radians

    scene_cls.camera_fov = property(_fov_get, _fov_set)
    scene_cls._nve_camera_fov_compat = True


def _apply_fvdb_point_cloud_compat():
    import fvdb.viz._point_cloud_view as point_cloud_view_module
    import torch

    point_cloud_view_cls = point_cloud_view_module.PointCloudView
    if getattr(point_cloud_view_cls, "__nanovdb_editor_point_cloud_compat__", False):
        return

    gaussian_splat_cls = point_cloud_view_module.GaussianSplat3d
    get_viewer_server_cpp = point_cloud_view_module._get_viewer_server_cpp

    def _add_gaussian_splat_view_compat(
        *,
        server,
        scene_name: str,
        name: str,
        means,
        quats,
        log_scales,
        logit_opacities,
        sh0,
        shN,
    ):
        # `fvdb-core@main` switched `add_gaussian_splat_3d_view()` to take raw
        # tensors directly, while older releases expected a wrapped
        # `GaussianSplat3d` implementation object.
        try:
            return server.add_gaussian_splat_3d_view(
                scene_name=scene_name,
                name=name,
                means=means,
                quats=quats,
                log_scales=log_scales,
                logit_opacities=logit_opacities,
                sh0=sh0,
                shN=shN,
            )
        except TypeError:
            gaussian_splat = gaussian_splat_cls.from_tensors(
                means=means,
                quats=quats,
                log_scales=log_scales,
                logit_opacities=logit_opacities,
                sh0=sh0,
                shN=shN,
            )

        gaussian_splat_impl = getattr(gaussian_splat, "_impl", gaussian_splat)
        try:
            return server.add_gaussian_splat_3d_view(
                scene_name=scene_name,
                name=name,
                gaussian_splat_3d=gaussian_splat_impl,
            )
        except TypeError:
            return server.add_gaussian_splat_3d_view(
                scene_name,
                name,
                gaussian_splat_impl,
            )

    def _compat_point_cloud_init(
        self,
        scene_name: str,
        name: str,
        positions,
        colors,
        point_size: float,
        _private=None,
    ):
        if _private is not self.__PRIVATE__:
            raise ValueError("PointCloudView constructor is private. Use Viewer.register_point_cloud_view() instead.")

        self._name = name
        self._scene_name = scene_name

        server = get_viewer_server_cpp()

        def _rgb_to_sh(rgb: torch.Tensor) -> torch.Tensor:
            c0 = 0.28209479177387814
            return (rgb - 0.5) / c0

        means = positions
        quats = torch.zeros((positions.shape[0], 4), dtype=torch.float32)
        quats[:, 0] = 1.0
        logit_opacities = torch.full((positions.shape[0],), 10.0, dtype=torch.float32)
        log_scales = torch.full((positions.shape[0], 3), -20.0, dtype=torch.float32)
        sh0 = _rgb_to_sh(colors)
        shN = torch.zeros((positions.shape[0], 0, 3), dtype=torch.float32)

        view = _add_gaussian_splat_view_compat(
            server=server,
            scene_name=scene_name,
            name=name,
            means=means,
            quats=quats,
            log_scales=log_scales,
            logit_opacities=logit_opacities,
            sh0=sh0,
            shN=shN,
        )
        view.tile_size = 16
        view.min_radius_2d = 0.0
        view.eps_2d = point_size / 2.0
        view.antialias = False
        view.sh_degree_to_use = 0

    point_cloud_view_cls.__init__ = _compat_point_cloud_init
    point_cloud_view_cls.__nanovdb_editor_point_cloud_compat__ = True


def _run_upstream_viz_suite(env_ctx: dict):
    python_exe = env_ctx["python"]
    env = env_ctx["env"]
    upstream_test_path = env_ctx["upstream_test_path"]
    _assert_viz_server_initializes(env_ctx)
    runner = """
import os
import sys
import traceback

import pytest

from pytests.test_fvdb_viz_integration import _apply_fvdb_camera_fov_compat, _apply_fvdb_point_cloud_compat

exit_code = 1
try:
    _apply_fvdb_camera_fov_compat()
    _apply_fvdb_point_cloud_compat()
    exit_code = int(pytest.main(sys.argv[1:]))
except BaseException:
    traceback.print_exc()
finally:
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(exit_code)
"""
    cmd = [
        str(python_exe),
        "-c",
        runner,
        str(upstream_test_path),
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
    local_spec = _resolved_local_path(LOCAL_DIST_SPEC)
    if not local_spec.exists():
        raise AssertionError(f"FVDB_VIZ_LOCAL_DIST target does not exist: {local_spec}")

    _install_nanovdb_distribution(fvdb_viz_env, str(local_spec))
    _run_upstream_viz_suite(fvdb_viz_env)
