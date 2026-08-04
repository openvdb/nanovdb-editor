# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Smoke tests for the Python pipeline helpers.

Two layers are covered:

- The pure-Python pipeline registry (``list_pipelines`` / ``get_pipeline_info``),
  which needs no GPU.
- The Scene conversion helpers (``nanovdb_from_gaussians`` for both the
  ``raster3d`` and ``voxelbvh`` processes, ``nanovdb_from_mesh`` and
  ``nanovdb_from_lines``) plus the ``VoxelBVH`` wrapper, which require a Vulkan
  device. Conversion coverage uses one shared editor session (same pattern as
  ``test_editor_api_2``) so CI does not pay for repeated Python/device init.
  The heaviest builds (``raster3d``, multi-direction RGBA8) stay behind
  ``NANOVDB_EDITOR_RUN_HEAVY_PIPELINE_TESTS=1`` on GitHub Actions.
"""

import gc
import os

import numpy as np
import pytest

import nanovdb_editor as nve  # type: ignore


def _skip_heavy_pipeline_conversions() -> bool:
    """Skip raster3d / multi-bake on software-Vulkan CI unless forced."""
    if os.environ.get("NANOVDB_EDITOR_RUN_HEAVY_PIPELINE_TESTS", "0") == "1":
        return False
    if os.environ.get("NANOVDB_EDITOR_RUN_PIPELINE_CONVERSION_TESTS", "0") == "1":
        return False
    return os.environ.get("GITHUB_ACTIONS") == "true"


_HEAVY_CONVERSION_REASON = (
    "Heavy Gaussian raster / multi-direction RGBA8 builds are skipped on "
    "GitHub Actions software-Vulkan runners; set "
    "NANOVDB_EDITOR_RUN_HEAVY_PIPELINE_TESTS=1 to force-run"
)


# --------------------------------------------------------------------------- #
# Registry (no GPU required)
# --------------------------------------------------------------------------- #


class TestPipelineRegistry:
    def test_list_all(self):
        pipelines = nve.list_pipelines()
        # Mirrors editor/PipelineTypes.h (pnanovdb_pipeline_type_count == 18).
        assert len(pipelines) == 18
        assert all(isinstance(p, nve.PipelineInfo) for p in pipelines)

    def test_list_by_stage(self):
        process = nve.list_pipelines("process")
        type_ids = {p.type_id for p in process}
        assert {"gaussian_voxelize", "voxelbvh_build"} <= type_ids
        # Render/load pipelines must not appear in the process stage.
        assert "gaussian_splat" not in type_ids
        assert all(p.stage == nve.PIPELINE_STAGE_PROCESS for p in process)

    def test_lookup_by_alias_value_and_enum_name(self):
        by_alias = nve.get_pipeline_info("raster3d")
        assert by_alias is not None
        assert by_alias.type_id == "gaussian_voxelize"
        assert by_alias.value == 3

        by_value = nve.get_pipeline_info(9)
        assert by_value.type_id == "voxelbvh_build"
        assert by_value.process == "voxelbvh"

        by_enum = nve.get_pipeline_info("pnanovdb_pipeline_type_nanovdb_surface")
        assert by_enum is not None
        assert by_enum.type_id == "nanovdb_surface"
        assert by_enum.enum_name == "pnanovdb_pipeline_type_nanovdb_surface"

    def test_lookup_unknown_returns_none(self):
        assert nve.get_pipeline_info("does_not_exist") is None
        assert nve.get_pipeline_info(9999) is None

    def test_defaults_exposed(self):
        assert nve.DEFAULT_VOXEL_SIZE == pytest.approx(1.0 / 128.0)
        assert nve.DEFAULT_VOXELS_PER_UNIT == pytest.approx(128.0)
        assert nve.DEFAULT_BVH_RESOLUTION == 512
        assert nve.MAX_BVH_RESOLUTION == 4096

    def test_rgba8_defaults_exposed(self):
        assert nve.PNANOVDB_GRID_TYPE_RGBA8 == 12
        assert nve.DEFAULT_RGBA8_UPSAMPLE == 2
        assert nve.MAX_RGBA8_UPSAMPLE == 4
        assert nve.DEFAULT_RGBA8_RAY_DIRECTION == (0.0, 0.0, -1.0)
        assert len(nve.DEFAULT_RGBA8_DIRECTIONS) == 8
        assert nve.DEFAULT_RGBA8_DIRECTIONS[0] == (-1.0, -1.0, 0.0)

    def test_resolve_pipeline_type(self):
        # int passthrough, process alias, enum name, and PipelineInfo all resolve.
        resolve = nve.Editor._resolve_pipeline_type
        assert resolve(9) == 9
        assert resolve("voxelbvh") == 9
        assert resolve("voxelbvh_build") == 9
        assert resolve("pnanovdb_pipeline_type_gaussian_voxelize") == 3
        assert resolve(nve.get_pipeline_info("raster3d")) == 3

    def test_resolve_pipeline_type_unknown(self):
        with pytest.raises(ValueError, match="Unknown pipeline"):
            nve.Editor._resolve_pipeline_type("does_not_exist")

    def test_pipeline_enums(self):
        # Stage enum values match the C stage ints and the legacy aliases.
        assert int(nve.PipelineStage.RENDER) == 2
        assert nve.PIPELINE_STAGE_PROCESS == nve.PipelineStage.PROCESS == 1
        # Pipeline type enum is generated from the registry.
        assert nve.PipelineType.voxelbvh_build == 9
        assert nve.PipelineType.gaussian_voxelize == 3
        info = nve.get_pipeline_info("voxelbvh")
        assert int(info) == 9
        assert info.type is nve.PipelineType.voxelbvh_build
        assert info.stage_enum is nve.PipelineStage.PROCESS

    def test_typed_params_are_scoped_to_their_process_pipeline(self):
        """Each helper must only touch the struct its process pipeline owns."""
        session = nve.create_default(device=False)
        try:
            scene = session.scene("main")

            scene.set_pipeline("bvh", nve.PipelineStage.PROCESS, "voxelbvh")
            scene.set_resolution("bvh", 256)
            scene.set_inflation_radius("bvh", 1.5)
            assert scene.get_resolution("bvh") == 256
            assert scene.get_inflation_radius("bvh") == pytest.approx(1.5)
            with pytest.raises(nve.PipelineError):
                scene.set_voxels_per_unit("bvh", 64.0)
            with pytest.raises(nve.PipelineError):
                scene.set_rgba8_bake_params("bvh", upsample_factor=4)
            # A rejected write must leave VoxelBVHBuildParams intact.
            assert scene.get_resolution("bvh") == 256
            assert scene.get_inflation_radius("bvh") == pytest.approx(1.5)

            scene.set_pipeline("gauss", nve.PipelineStage.PROCESS, "raster3d")
            scene.set_voxels_per_unit("gauss", 64.0)
            assert scene.get_voxels_per_unit("gauss") == pytest.approx(64.0)
            with pytest.raises(nve.PipelineError):
                scene.set_resolution("gauss", 512)

            scene.set_pipeline("rgba8", nve.PipelineStage.PROCESS, "voxelbvh_rgba8")
            scene.set_rgba8_bake_params("rgba8", bake_all_directions=True, upsample_factor=3)
            assert scene.get_rgba8_bake_params("rgba8") == (True, nve.DEFAULT_RGBA8_RAY_DIRECTION, 3)
            with pytest.raises(nve.PipelineError):
                scene.set_resolution("rgba8", 512)
        finally:
            session.close()

    def test_rgba8_bake_params_fall_back_to_documented_defaults(self):
        session = nve.create_default(device=False)
        try:
            scene = session.scene("main")
            # No params mapped for an unknown object: report the editor defaults.
            bake_all, ray, upsample = scene.get_rgba8_bake_params("no_such_object")
            assert bake_all is False
            assert ray == nve.DEFAULT_RGBA8_RAY_DIRECTION
            assert upsample == nve.DEFAULT_RGBA8_UPSAMPLE
        finally:
            session.close()

    def test_exception_hierarchy(self):
        # Library errors derive from NanoVDBError and the closest built-in.
        assert issubclass(nve.PipelineError, nve.NanoVDBError)
        assert issubclass(nve.PipelineError, RuntimeError)
        assert issubclass(nve.InvalidArgumentError, nve.NanoVDBError)
        assert issubclass(nve.InvalidArgumentError, ValueError)
        assert issubclass(nve.DeviceError, nve.NanoVDBError)
        assert issubclass(nve.SessionClosedError, nve.NanoVDBError)
        assert issubclass(nve.SessionClosedError, RuntimeError)

    def test_make_editor_config_kwargs(self):
        cfg = nve.make_editor_config(ip="10.0.0.1", port=9000, headless=True, streaming=True)
        assert cfg.ip_address == b"10.0.0.1"
        assert cfg.port == 9000
        assert cfg.headless == 1
        assert cfg.streaming == 1

    def test_make_editor_config_copies_string_storage(self):
        source = nve.make_editor_config(ip="10.0.0.7", ui_profile="profile-x")
        cfg = nve.make_editor_config(source, port=9999)
        assert cfg.port == 9999
        assert cfg.ip_address == b"10.0.0.7"
        assert cfg.ui_profile_name == b"profile-x"
        assert cfg._keepalive
        assert cfg.ip_address in cfg._keepalive
        assert cfg.ui_profile_name in cfg._keepalive

    def test_raster_from_arrays_requires_six_inputs(self):
        from unittest.mock import MagicMock

        from nanovdb_editor.raster import Raster

        # Validation runs before native state is accessed.
        raster = Raster.__new__(Raster)
        with pytest.raises(nve.InvalidArgumentError, match="6 arrays"):
            raster.raster_to_nanovdb_from_arrays(0.01, [MagicMock()] * 5)

    def test_process_chain_append_reports_native_rejection(self):
        session = nve.create_default(device=False)
        try:
            scene = session.scene("main")
            scene.set_pipeline("obj", nve.PipelineStage.PROCESS, "voxelbvh")
            chain = scene.process_steps("obj")
            with pytest.raises(nve.PipelineError):
                chain.append("voxelbvh_rgba8_chain")
            assert len(chain) == 1
        finally:
            session.close()

    def test_default_mesh_colors_size_non_numpy_positions(self):
        """``colors=None`` must size white colors from any accepted positions input."""
        session = nve.create_default(device=False)
        try:
            positions = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], dtype=np.float32)
            assert nve.Scene._position_float_count(positions) == 9

            flat = positions.reshape(-1)
            raw = session.compute.create_array(flat)
            try:
                assert nve.Scene._position_float_count(raw) == 9
                assert nve.Scene._position_float_count(nve.Grid(session.compute, raw)) == 9
            finally:
                session.compute.destroy_array(raw)

            with session.compute.array(flat) as owned:
                assert nve.Scene._position_float_count(owned) == 9
        finally:
            session.close()

    def test_owned_array_context_manager(self):
        session = nve.create_default(device=False)
        try:
            data = np.arange(8, dtype=np.uint32)
            with session.compute.array(data) as arr:
                assert arr.element_count == 8
                assert arr.element_size == 4
            with pytest.raises(nve.InvalidArgumentError):
                _ = arr.raw
        finally:
            session.close()


# --------------------------------------------------------------------------- #
# Conversion helpers (require a Vulkan device)
# --------------------------------------------------------------------------- #


def _make_gaussians(num_points=32):
    """Build a small but well-conditioned Gaussian cloud.

    Values mirror the raw, on-disk convention consumed by the pipelines:
    log-space ``scales`` and logit-space ``opacities`` (both transformed inside
    native code), unit quaternions, and higher-order SH stored as 15 RGB
    coefficients per point. A degenerate/too-sparse cloud can trip the native
    rasterizer, so keep points clustered and scales modest.

    Keep the cloud small: CI runs these conversions on a software Vulkan
    device, where the rasterized grid has to stay well within runner memory.
    """
    rng = np.random.default_rng(0)
    means = (rng.standard_normal((num_points, 3)) * 0.3).astype(np.float32)
    opacities = np.full((num_points, 1), 0.9, dtype=np.float32)
    quats = np.tile([1.0, 0.0, 0.0, 0.0], (num_points, 1)).astype(np.float32)
    scales = np.full((num_points, 3), -3.0, dtype=np.float32)  # log-space -> ~0.05
    sh_0 = np.full((num_points, 3), 0.5, dtype=np.float32)
    sh_n = np.zeros((num_points, 45), dtype=np.float32)  # 15 higher-order RGB coeffs
    return dict(means=means, quats=quats, scales=scales, sh_0=sh_0, sh_n=sh_n, opacities=opacities)


# Coarse on purpose: grid cost scales with 1/voxel_size and resolution^3.
_RASTER3D_VOXEL_SIZE = 1.0 / 8.0
_VOXELBVH_RESOLUTION = 8


def _make_tiny_mesh():
    """Minimal triangle mesh used as a cheap VoxelBVH / RGBA8 source."""
    positions = np.array(
        [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]],
        dtype=np.float32,
    )
    indices = np.array([0, 1, 2], dtype=np.uint32)
    return positions, indices


class TestPipelineConversions:
    @pytest.fixture(scope="class", autouse=True)
    def pipeline_conversion_resources(self, request):
        """One shared Session for all conversion tests (mirrors test_editor_api_2).

        Device/Python init dominates CI time; recreating it per method was enough
        to exhaust software-Vulkan runners even with tiny meshes.
        """
        try:
            session = nve.create_default()
        except Exception as exc:
            request.cls._pipeline_conversion_skip = f"No compute device available: {exc}"
            yield
            return

        request.cls._pipeline_conversion_skip = None
        request.cls.session = session
        request.cls.editor, request.cls.compute, request.cls.compiler = session
        print("Initialized shared pipeline conversion fixture")

        yield

        try:
            session.close()
        except Exception:
            pass
        request.cls.session = None
        request.cls.editor = None
        request.cls.compute = None
        request.cls.compiler = None
        gc.collect()

    def setup_method(self):
        skip = getattr(self, "_pipeline_conversion_skip", None)
        if skip:
            pytest.skip(skip)

    def _assert_valid_grid(self, nvdb, *, destroy=True):
        assert nvdb is not None
        assert nvdb.element_count > 0
        if not destroy:
            return
        if isinstance(nvdb, nve.Grid):
            nvdb.close()
        else:
            self.compute.destroy_array(nvdb)

    def test_pipeline_conversions_smoke(self):
        """Single-run smoke: mesh/lines/gaussians/RGBA8/process/render/numpy.

        Kept as one method so CI pays for one device lifetime and a small
        number of builds, matching the Editor API 2 fixture pattern.
        """
        scene = self.editor.scene("main")
        positions, indices = _make_tiny_mesh()

        # Mesh → VoxelBVH, numpy round-trip, render pipeline, process chain.
        mesh = scene.nanovdb_from_mesh(
            indices=indices,
            positions=positions,
            resolution=_VOXELBVH_RESOLUTION,
            name="mesh",
        )
        try:
            arr = mesh.to_numpy()
            assert isinstance(arr, np.ndarray)
            assert arr.size == mesh.element_count

            expected = nve.get_pipeline_info("voxelbvh_triangles_render")
            scene.set_render_pipeline("mesh", "voxelbvh_triangles_render")
            assert scene.get_render_pipeline("mesh") is expected

            steps = scene.process_steps("mesh")
            steps[0] = "voxelbvh"
            rgba_step = steps.append("voxelbvh_rgba8")
            assert len(steps) >= 2
            assert steps[0].pipeline is nve.get_pipeline_info("voxelbvh")
            assert rgba_step.pipeline is nve.get_pipeline_info("voxelbvh_rgba8")
            assert [s.type_id for s in steps][:2] == ["voxelbvh_build", "voxelbvh_rgba8"]
            with rgba_step.params() as params:
                assert params is None or (params.size >= 0)
            assert steps[-1].type_id == "voxelbvh_rgba8"
            with pytest.raises(IndexError):
                _ = steps[len(steps)]

            with scene.nanovdb_to_rgba8(
                mesh, name="rgba8", upsample_factor=1, add=False
            ) as rgba8:
                assert rgba8.element_count > 0

            with pytest.raises(ValueError, match="upsample_factor"):
                scene.nanovdb_to_rgba8(mesh, upsample_factor=99, add=False)
        finally:
            mesh.close()

        # Lines → VoxelBVH.
        line_positions = np.array(
            [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [1.0, 1.0, 0.0]],
            dtype=np.float32,
        )
        line_indices = np.array([0, 1, 1, 2], dtype=np.uint32)
        lines = scene.nanovdb_from_lines(
            indices=line_indices,
            positions=line_positions,
            resolution=_VOXELBVH_RESOLUTION,
            add=False,
        )
        self._assert_valid_grid(lines)

        # Gaussians → VoxelBVH with sh_n=None (empty higher-order SH).
        gaussians = _make_gaussians()
        gaussians["sh_n"] = None
        nvdb = scene.nanovdb_from_gaussians(
            **gaussians,
            process="voxelbvh",
            resolution=_VOXELBVH_RESOLUTION,
            register=False,
        )
        self._assert_valid_grid(nvdb)

        # Low-level VoxelBVH wrapper path.
        voxelbvh = self.editor._get_voxelbvh()
        assert isinstance(voxelbvh, nve.VoxelBVH)
        gaussians = _make_gaussians()
        arrays = [
            self.compute.create_array(gaussians["means"]),
            self.compute.create_array(gaussians["opacities"]),
            self.compute.create_array(gaussians["quats"]),
            self.compute.create_array(gaussians["scales"]),
            self.compute.create_array(gaussians["sh_0"]),
            self.compute.create_array(gaussians["sh_n"]),
        ]
        try:
            nvdb = voxelbvh.nanovdb_from_gaussians_array(
                arrays, resolution=_VOXELBVH_RESOLUTION
            )
            self._assert_valid_grid(nvdb)
        finally:
            for array in arrays:
                self.compute.destroy_array(array)

    @pytest.mark.skipif(_skip_heavy_pipeline_conversions(), reason=_HEAVY_CONVERSION_REASON)
    def test_gaussians_raster3d(self):
        scene = self.editor.scene("main")
        nvdb = scene.nanovdb_from_gaussians(
            **_make_gaussians(),
            process=nve.PipelineType.gaussian_voxelize,
            voxel_size=_RASTER3D_VOXEL_SIZE,
            register=False,
        )
        self._assert_valid_grid(nvdb)

    @pytest.mark.skipif(_skip_heavy_pipeline_conversions(), reason=_HEAVY_CONVERSION_REASON)
    def test_mesh_to_rgba8_directions(self):
        scene = self.editor.scene("main")
        positions, indices = _make_tiny_mesh()
        grid = scene.nanovdb_from_mesh(
            indices=indices,
            positions=positions,
            resolution=_VOXELBVH_RESOLUTION,
            add=False,
        )
        try:
            directions = [(0.0, 0.0, -1.0), (0.0, 0.0, 1.0)]
            grids = scene.nanovdb_to_rgba8_directions(
                grid, name="rgba8", directions=directions, upsample_factor=1, add=False
            )
            try:
                assert len(grids) == 2
                for g in grids:
                    assert g.element_count > 0
            finally:
                for g in grids:
                    g.close()
        finally:
            grid.close()


# --------------------------------------------------------------------------- #
# Argument validation (no GPU required)
# --------------------------------------------------------------------------- #


class TestPipelineValidation:
    @pytest.fixture(autouse=True)
    def setup_and_teardown(self):
        # Argument validation only — no GPU device required.
        self.session = nve.create_default(device=False)
        self.editor, self.compute, self.compiler = self.session
        yield
        self.session.close()
        self.editor = None
        self.compute = None
        self.compiler = None
        gc.collect()

    def test_unsupported_gaussian_process(self):
        scene = self.editor.scene("main")
        with pytest.raises(ValueError, match="Unsupported process"):
            scene.nanovdb_from_gaussians(
                **_make_gaussians(),
                process="not_a_pipeline",
                add=False,
            )

    def test_unsupported_mesh_process(self):
        scene = self.editor.scene("main")
        positions = np.zeros((3, 3), dtype=np.float32)
        indices = np.array([0, 1, 2], dtype=np.uint32)
        with pytest.raises(ValueError, match="Unsupported process"):
            scene.nanovdb_from_mesh(
                indices=indices,
                positions=positions,
                process="not_a_pipeline",
                add=False,
            )

    def test_rgba8_directions_empty_raises(self):
        scene = self.editor.scene("main")
        with pytest.raises(ValueError, match="directions"):
            # Validation runs before the source is touched.
            scene.nanovdb_to_rgba8_directions(src=None, directions=[], add=False)
