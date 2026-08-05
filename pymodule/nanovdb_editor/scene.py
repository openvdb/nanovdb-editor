# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""High-level, scene-scoped helpers for building NanoVDB grids.

A :class:`Scene` bundles a scene token with convenience helpers that build
NanoVDB grids from higher-level inputs (Gaussian splats, triangle meshes, line
sets) using the editor's existing process pipelines and register the result
with the scene. Obtain one via :meth:`nanovdb_editor.editor.Editor.scene`.

``Editor`` is only imported under ``TYPE_CHECKING`` here; the runtime dependency
is one-way (``Editor.scene`` lazily imports this module) to avoid an import
cycle.
"""

import math
import os
from ctypes import POINTER, c_float, c_int32, c_uint32, cast
from typing import TYPE_CHECKING, Iterator, List, Optional, Sequence, Tuple, Union

import numpy as np

from .compute import Array, pnanovdb_ComputeArray
from .exceptions import InvalidArgumentError, PipelineError
from .grid import Grid
from .pipelines import (
    DEFAULT_BVH_RESOLUTION,
    DEFAULT_VOXELS_PER_UNIT,
    MAX_BVH_RESOLUTION,
    PIPELINE_STAGE_PROCESS,
    PIPELINE_STAGE_RENDER,
    PipelineInfo,
    PipelineStage,
    PipelineType,
    get_pipeline_info,
)
from .voxelbvh import (
    DEFAULT_RGBA8_RAY_DIRECTION,
    DEFAULT_RGBA8_UPSAMPLE,
    MAX_RGBA8_UPSAMPLE,
    normalize_rgba8_directions,
)

if TYPE_CHECKING:
    from .editor import Editor

GridInput = Union[Grid, pnanovdb_ComputeArray]

# GaussianVoxelizeParams.voxels_per_unit is clamped to this range on the C++ side.
_VOXELS_PER_UNIT_MIN = 1.0
_VOXELS_PER_UNIT_MAX = 512.0


class ProcessStep:
    """A live handle to one step in an object's process chain.

    Obtained by indexing a :class:`ProcessChain`::

        step = scene.process_steps("mesh")[1]
        print(step.pipeline.type_id)
        step.pipeline = "voxelbvh_rgba8"
        with step.params() as p:
            ...
    """

    __slots__ = ("_chain", "_index")

    def __init__(self, chain: "ProcessChain", index: int):
        self._chain = chain
        self._index = int(index)

    @property
    def index(self) -> int:
        """Zero-based position of this step in the chain."""
        return self._index

    @property
    def pipeline(self) -> Optional[PipelineInfo]:
        """The pipeline bound to this step (or ``None`` if unknown/noop)."""
        return self._chain._get(self._index)

    @pipeline.setter
    def pipeline(self, value) -> None:
        self._chain[self._index] = value

    @property
    def type_id(self) -> Optional[str]:
        """Shortcut for ``step.pipeline.type_id`` (or ``None``)."""
        info = self.pipeline
        return None if info is None else info.type_id

    def params(self):
        """Context manager over this step's mapped parameter block."""
        return self._chain.params(self._index)

    def __repr__(self) -> str:
        info = self.pipeline
        type_id = "?" if info is None else info.type_id
        return f"ProcessStep(index={self._index}, pipeline={type_id!r})"


class ProcessChain:
    """Mutable, sequence-like view of an object's process-step chain.

    Returned by :meth:`Scene.process_steps`. Supports ``len``, indexing,
    assignment, iteration and ``append`` — the natural Python shape for a
    growing list of pipelines::

        steps = scene.process_steps("mesh")
        steps[0] = "voxelbvh"
        steps.append("voxelbvh_rgba8")
        print([s.type_id for s in steps])
        with steps[1].params() as p:
            ...
    """

    __slots__ = ("_editor", "_scene_token", "_name", "_name_token")

    def __init__(self, editor: "Editor", scene_token, name: str):
        self._editor = editor
        self._scene_token = scene_token
        self._name = name
        self._name_token = editor.get_token(name)

    @property
    def name(self) -> str:
        """Object name this chain belongs to."""
        return self._name

    def __len__(self) -> int:
        return self._editor.get_process_step_count(self._scene_token, self._name_token)

    def __getitem__(self, index) -> Union[ProcessStep, List[ProcessStep]]:
        if isinstance(index, slice):
            return [ProcessStep(self, i) for i in range(*index.indices(len(self)))]
        return ProcessStep(self, self._normalize_index(index, allow_append=False))

    def __setitem__(self, index: int, pipeline) -> None:
        if isinstance(index, slice):
            raise TypeError("ProcessChain slice assignment is not supported")
        self._editor.set_process_step(
            self._scene_token,
            self._name_token,
            self._normalize_index(index, allow_append=True),
            pipeline,
        )

    def __iter__(self) -> Iterator[ProcessStep]:
        for i in range(len(self)):
            yield ProcessStep(self, i)

    def __repr__(self) -> str:
        type_ids = [s.type_id for s in self]
        return f"ProcessChain(name={self._name!r}, steps={type_ids!r})"

    def append(self, pipeline) -> ProcessStep:
        """Append a process pipeline and return the new :class:`ProcessStep`."""
        index = len(self)
        self._editor.set_process_step(self._scene_token, self._name_token, index, pipeline)
        return ProcessStep(self, index)

    def params(self, index: int):
        """Context manager over the mapped parameters of step ``index``."""
        return self._editor.process_step_params(
            self._scene_token, self._name_token, self._normalize_index(index, allow_append=False)
        )

    def _get(self, index: int) -> Optional[PipelineInfo]:
        value = self._editor.get_process_step(self._scene_token, self._name_token, index)
        return get_pipeline_info(value)

    def _normalize_index(self, index: int, *, allow_append: bool) -> int:
        index = int(index)
        count = len(self)
        if index < 0:
            index += count
        upper = count if allow_append else count - 1
        if count == 0 and not allow_append:
            raise IndexError("process chain is empty")
        if index < 0 or index > upper:
            raise IndexError(f"process step index out of range for chain of length {count}")
        return index


class Scene:
    """High-level handle to a named scene in the editor.

    Obtain an instance via :meth:`Editor.scene`. It bundles the scene token
    together with convenience helpers that build NanoVDB grids from
    higher-level inputs (Gaussian splats, triangle meshes, line sets) using the
    editor's existing process pipelines and register the result with the scene.

    Each ``nanovdb_from_*`` / ``nanovdb_to_*`` helper accepts either NumPy arrays
    or pre-built ``pnanovdb_ComputeArray`` instances, and dispatches to the
    pipeline selected by ``process`` (see :mod:`nanovdb_editor.pipelines`).
    These conversion helpers require a GPU compute device (they call into
    Raster / VoxelBVH); CPU-only sessions must mock those paths or create a
    device first.
    """

    # Supported ``process`` values for building NanoVDB grids from Gaussians.
    _GAUSSIAN_PROCESSES = ("raster3d", "voxelbvh")
    _GAUSSIAN_PROCESS_ALIASES = {
        "raster3d": "raster3d",
        "voxelbvh": "voxelbvh",
        "gaussian_voxelize": "raster3d",
        "voxelbvh_build": "voxelbvh",
        PipelineType.gaussian_voxelize: "raster3d",
        PipelineType.voxelbvh_build: "voxelbvh",
    }

    # Process pipelines that own each typed parameter struct. Chains expand into
    # concrete steps, so the process stage always reports one of these.
    _GAUSSIAN_VOXELIZE_PIPELINES = ("gaussian_voxelize",)
    _VOXELBVH_BUILD_PIPELINES = ("voxelbvh_build",)
    _RGBA8_PIPELINES = ("voxelbvh_rgba8",)

    def __init__(self, editor: "Editor", name: str):
        self._editor = editor
        self.name = name
        self._token = editor.get_token(name)

    @property
    def token(self):
        """Advanced: underlying scene token (from ``editor.get_token``).

        Prefer name-based :class:`Scene` helpers; the token is for low-level
        Editor / FFI interop.
        """
        return self._token

    class _Arrays:
        """Tracks compute arrays created from NumPy inputs for cleanup.

        Prefer ``with Scene._Arrays(compute) as arrays:`` so owned buffers are
        always released, including when a conversion fails mid-flight.
        """

        def __init__(self, compute):
            self._compute = compute
            self._owned = []

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            self.destroy()
            return False

        def to_array(self, value, dtype=np.float32):
            if value is None:
                return None
            if isinstance(value, Grid):
                return value.array
            if isinstance(value, Array):
                return value.raw
            if isinstance(value, pnanovdb_ComputeArray):
                return value
            np_array = np.ascontiguousarray(value, dtype=dtype)
            compute_array = self._compute.create_array(np_array)
            # Keep the source buffer alive until the pipeline call completes.
            self._owned.append((compute_array, np_array))
            return compute_array

        def destroy(self):
            for compute_array, _ in self._owned:
                self._compute.destroy_array(compute_array)
            self._owned = []

    @staticmethod
    def _position_float_count(positions) -> int:
        """Number of float32 values in a positions buffer (``3 * vertex_count``)."""
        if isinstance(positions, Grid):
            positions = positions.array
        if isinstance(positions, Array):
            positions = positions.raw
        if isinstance(positions, pnanovdb_ComputeArray):
            if int(positions.element_size) != 4:
                raise InvalidArgumentError("positions must be float32")
            return int(positions.element_count)
        positions_np = np.ascontiguousarray(positions, dtype=np.float32)
        return int(positions_np.size)

    @staticmethod
    def _resolve_register(add: bool, register: Optional[bool]) -> bool:
        return add if register is None else bool(register)

    @classmethod
    def _resolve_gaussian_process(cls, process) -> str:
        if isinstance(process, PipelineInfo):
            process = process.process or process.type_id
        key = process
        if isinstance(process, PipelineType):
            key = process
        alias = cls._GAUSSIAN_PROCESS_ALIASES.get(key)
        if alias is None and isinstance(process, str):
            alias = cls._GAUSSIAN_PROCESS_ALIASES.get(process)
        if alias is None:
            raise InvalidArgumentError(f"Unsupported process {process!r}; supported: {cls._GAUSSIAN_PROCESSES}")
        return alias

    def _finalize(self, nvdb_array, name, add) -> Grid:
        if add:
            self._editor.add_nanovdb_2(self._token, self._editor.get_token(name), nvdb_array)
        return Grid(self._editor._compute, nvdb_array)

    def add_grid(self, grid, name: str) -> Grid:
        """Register an existing :class:`Grid` (or raw array) under ``name``."""
        if isinstance(grid, Array):
            array = grid.raw
        else:
            array = Grid.unwrap(grid)
        self._editor.add_nanovdb_2(self._token, self._editor.get_token(name), array)
        if isinstance(grid, Grid):
            return grid
        return Grid(self._editor._compute, array)

    def remove(self, name: str) -> None:
        """Remove an object from this scene."""
        self._editor.remove(self._token, self._editor.get_token(name))

    def load(self, filepath: str, overwrite: bool = False) -> None:
        """Load a scene file into the editor (affects all scenes)."""
        self._editor.load_scene(filepath, overwrite=overwrite)

    def save(self, filepath: str) -> None:
        """Save all editor scenes to ``filepath``."""
        self._editor.save_scene(filepath)

    def set_custom_params(self, json_string) -> None:
        """Attach scene-level custom UI params from a JSON string/bytes."""
        self._editor.set_custom_scene_params(self._token, json_string)

    def set_custom_params_from_file(self, filepath) -> None:
        """Attach scene-level custom params from a JSON file on disk."""
        self._editor.set_custom_scene_params_from_file(self._token, filepath)

    def reload_custom_params_if_changed(self, filepath=None) -> bool:
        """Re-apply custom params from disk if the JSON file changed."""
        return self._editor.reload_custom_scene_params_if_changed(self._token, filepath)

    def get_camera(self):
        """Return a copy of this scene's camera, or ``None`` if unknown."""
        return self._editor.get_camera_2(self._token)

    def update_camera(
        self,
        camera=None,
        *,
        position: Optional[Sequence[float]] = None,
        eye_direction: Optional[Sequence[float]] = None,
        eye_up: Optional[Sequence[float]] = None,
        eye_distance: Optional[float] = None,
    ):
        """Update this scene's camera.

        Pass a full :class:`~nanovdb_editor.Camera` value, or override selected
        state fields with plain Python sequences/floats.
        """
        from .editor import Camera, Vec3

        if camera is None:
            camera = self.get_camera()
            if camera is None:
                camera = Camera()
        if position is not None:
            x, y, z = position
            camera.state.position = Vec3(float(x), float(y), float(z))
        if eye_direction is not None:
            x, y, z = eye_direction
            camera.state.eye_direction = Vec3(float(x), float(y), float(z))
        if eye_up is not None:
            x, y, z = eye_up
            camera.state.eye_up = Vec3(float(x), float(y), float(z))
        if eye_distance is not None:
            camera.state.eye_distance_from_position = float(eye_distance)
        self._editor.update_camera_2(self._token, camera)
        return camera

    def add_image2d(self, name: str, image_rgba, width: Optional[int] = None, height: Optional[int] = None) -> None:
        """Add a 2D RGBA image to this scene.

        Args:
            name: Object name for the image.
            image_rgba: ``(H, W)`` ``uint32`` packed RGBA NumPy array, or a
                ``pnanovdb_ComputeArray`` / :class:`~nanovdb_editor.compute.Array`.
            width / height: Required when ``image_rgba`` is a raw compute array;
                inferred from the NumPy shape otherwise.
        """
        compute = self._editor._compute
        owned = None
        if isinstance(image_rgba, Array):
            array = image_rgba.raw
            if width is None or height is None:
                raise InvalidArgumentError("width and height are required for Array / compute-array inputs")
        elif isinstance(image_rgba, pnanovdb_ComputeArray):
            array = image_rgba
            if width is None or height is None:
                raise InvalidArgumentError("width and height are required for Array / compute-array inputs")
        else:
            np_image = np.ascontiguousarray(image_rgba, dtype=np.uint32)
            if np_image.ndim != 2:
                raise InvalidArgumentError("image_rgba must be a (H, W) uint32 array")
            height = int(np_image.shape[0]) if height is None else int(height)
            width = int(np_image.shape[1]) if width is None else int(width)
            owned = compute.create_array(np_image)
            array = owned
        try:
            self._editor.add_image2d(self._token, self._editor.get_token(name), array, int(width), int(height))
        finally:
            if owned is not None:
                compute.destroy_array(owned)

    def nanovdb_from_file(
        self,
        filepath: str,
        name: Optional[str] = None,
        add: bool = True,
        *,
        register: Optional[bool] = None,
    ) -> Grid:
        """Load an existing NanoVDB grid from a ``.nvdb`` file into the scene.

        Unlike the other ``nanovdb_from_*`` helpers, this does not run a process
        pipeline: it reads a pre-built grid straight from disk via
        :meth:`Compute.load_nanovdb`.

        Args:
            filepath: Path to the ``.nvdb`` file to load.
            name: Object name to register the grid under within the scene.
                Defaults to the file's base name (without extension).
            add / register: When True (default), also register the grid with the
                scene. Prefer ``register=``; ``add=`` is kept as an alias.
        """
        nvdb_array = self._editor._compute.load_nanovdb(filepath)
        if name is None:
            name = os.path.splitext(os.path.basename(filepath))[0] or "nanovdb"
        return self._finalize(nvdb_array, name, self._resolve_register(add, register))

    # ------------------------------------------------------------------
    # Pipeline assignment (how an object is loaded / processed / drawn)
    # ------------------------------------------------------------------

    def set_pipeline(self, name: str, stage: Union[int, PipelineStage], pipeline) -> None:
        """Assign a pipeline to one of an object's stages.

        Args:
            name: Object name within this scene.
            stage: A :class:`~nanovdb_editor.pipelines.PipelineStage`.
            pipeline: Pipeline enum value/``PipelineType``, ``process``/enum-name
                string, or a ``PipelineInfo`` (see
                :func:`nanovdb_editor.list_pipelines`).
        """
        self._editor.set_pipeline(self._token, self._editor.get_token(name), stage, pipeline)

    def get_pipeline(self, name: str, stage: Union[int, PipelineStage]) -> Optional[PipelineInfo]:
        """Return the :class:`PipelineInfo` bound to an object's stage.

        Returns ``None`` if the bound value is not a known pipeline type.
        """
        value = self._editor.get_pipeline(self._token, self._editor.get_token(name), stage)
        return get_pipeline_info(value)

    def set_render_pipeline(self, name: str, pipeline) -> None:
        """Choose how an object is drawn (its ``render`` pipeline).

        Example::

            scene.set_render_pipeline("mesh", "voxelbvh_triangles_render")
        """
        self.set_pipeline(name, PIPELINE_STAGE_RENDER, pipeline)

    def get_render_pipeline(self, name: str) -> Optional[PipelineInfo]:
        """Return the :class:`PipelineInfo` for how an object is drawn."""
        return self.get_pipeline(name, PIPELINE_STAGE_RENDER)

    def mark_pipeline_dirty(self, name: str) -> None:
        """Force an object's pipelines to re-run on the next editor update."""
        self._editor.mark_pipeline_dirty(self._token, self._editor.get_token(name))

    def pipeline_params(self, name: str, stage: Union[int, PipelineStage]):
        """Context manager over an object's parameters for a pipeline stage.

        Yields the mapped ``pnanovdb_pipeline_params_t`` (or ``None`` when the
        stage exposes no parameters) and flushes writes / marks the stage dirty
        on exit. Prefer the typed helpers (:meth:`set_voxels_per_unit`,
        :meth:`set_resolution`, …) when possible.
        """
        return self._editor.pipeline_params(self._token, self._editor.get_token(name), stage)

    # ------------------------------------------------------------------
    # Multi-step process chains
    # ------------------------------------------------------------------

    def process_steps(self, name: str) -> ProcessChain:
        """Return a sequence-like view of an object's process-step chain.

        The returned :class:`ProcessChain` supports ``len``, indexing,
        assignment, iteration and ``append``::

            steps = scene.process_steps("mesh")
            steps[0] = "voxelbvh"
            steps.append("voxelbvh_rgba8")
            print([s.type_id for s in steps])
            with steps[1].params() as p:
                ...
        """
        return ProcessChain(self._editor, self._token, name)

    # ------------------------------------------------------------------
    # Per-pipeline parameters
    # ------------------------------------------------------------------

    def get_voxels_per_unit(self, name: str) -> float:
        """Read the Gaussian-voxelize density (voxels per world unit) of an object.

        Returns the default (``128``) when the object exposes no process
        parameters, or a process pipeline that has no such parameter.
        """
        if not self._owns_process_params(name, self._GAUSSIAN_VOXELIZE_PIPELINES):
            return DEFAULT_VOXELS_PER_UNIT
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            if params is not None and params.data and params.size >= 4:
                value = cast(params.data, POINTER(c_float)).contents.value
                return self._clamp_voxels_per_unit(value)
        return DEFAULT_VOXELS_PER_UNIT

    def set_voxels_per_unit(self, name: str, value: float) -> None:
        """Set the Gaussian-voxelize density (voxels per world unit) of an object.

        The value is clamped to ``[1, 512]`` to match the editor.

        Raises:
            PipelineError: If the object has no writable process parameters, or
                its process pipeline is not a Gaussian voxelize.
        """
        self._require_process_params(name, self._GAUSSIAN_VOXELIZE_PIPELINES, "voxels_per_unit")
        value = self._clamp_voxels_per_unit(value)
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            if params is not None and params.data and params.size >= 4:
                cast(params.data, POINTER(c_float))[0] = value
                return
        raise PipelineError(f"Object {name!r} has no process parameters for voxels_per_unit")

    def get_resolution(self, name: str) -> int:
        """Read VoxelBVH build resolution (defaults to ``DEFAULT_BVH_RESOLUTION``)."""
        if not self._owns_process_params(name, self._VOXELBVH_BUILD_PIPELINES):
            return int(DEFAULT_BVH_RESOLUTION)
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            # VoxelBVHBuildParams: source_type u32 @0, resolution u32 @4, inflation f32 @8
            if params is not None and params.data and params.size >= 12:
                return int(cast(params.data, POINTER(c_uint32))[1])
        return int(DEFAULT_BVH_RESOLUTION)

    def set_resolution(self, name: str, value: int) -> None:
        """Set VoxelBVH build resolution (clamped to ``1..MAX_BVH_RESOLUTION``).

        Raises:
            PipelineError: If the object's process pipeline is not a VoxelBVH
                build, or it has no writable process parameters.
        """
        self._require_process_params(name, self._VOXELBVH_BUILD_PIPELINES, "resolution")
        value = max(1, min(int(MAX_BVH_RESOLUTION), int(value)))
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            if params is not None and params.data and params.size >= 12:
                cast(params.data, POINTER(c_uint32))[1] = value
                return
        raise PipelineError(f"Object {name!r} has no process parameters for resolution")

    def get_inflation_radius(self, name: str) -> float:
        """Read VoxelBVH inflation radius (``0`` means auto)."""
        if not self._owns_process_params(name, self._VOXELBVH_BUILD_PIPELINES):
            return 0.0
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            if params is not None and params.data and params.size >= 12:
                return float(cast(params.data, POINTER(c_float))[2])
        return 0.0

    def set_inflation_radius(self, name: str, value: float) -> None:
        """Set VoxelBVH inflation radius (``0`` means auto).

        Raises:
            PipelineError: If the object's process pipeline is not a VoxelBVH
                build, or it has no writable process parameters.
        """
        self._require_process_params(name, self._VOXELBVH_BUILD_PIPELINES, "inflation_radius")
        value = float(value)
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            if params is not None and params.data and params.size >= 12:
                cast(params.data, POINTER(c_float))[2] = value
                return
        raise PipelineError(f"Object {name!r} has no process parameters for inflation_radius")

    def get_rgba8_bake_params(self, name: str) -> Tuple[bool, Tuple[float, float, float], int]:
        """Read RGBA8 bake params ``(bake_all_directions, ray_dir, upsample)``.

        Layout matches ``VoxelBVHRgba8Params`` on the process stage. Objects
        whose process pipeline is not an RGBA8 bake report the editor defaults.
        """
        if not self._owns_process_params(name, self._RGBA8_PIPELINES):
            return False, DEFAULT_RGBA8_RAY_DIRECTION, DEFAULT_RGBA8_UPSAMPLE
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            # bake_all i32 @0, ray xyz f32 @4/8/12, upsample u32 @16
            if params is not None and params.data and params.size >= 20:
                base = params.data
                bake_all = bool(cast(base, POINTER(c_int32))[0])
                floats = cast(base, POINTER(c_float))
                ray = (float(floats[1]), float(floats[2]), float(floats[3]))
                upsample = int(cast(base, POINTER(c_uint32))[4])
                return bake_all, ray, upsample
        # No params mapped yet: report the same defaults the editor would apply.
        return False, DEFAULT_RGBA8_RAY_DIRECTION, DEFAULT_RGBA8_UPSAMPLE

    def set_rgba8_bake_params(
        self,
        name: str,
        *,
        bake_all_directions: Optional[bool] = None,
        ray_direction: Optional[Sequence[float]] = None,
        upsample_factor: Optional[int] = None,
    ) -> None:
        """Write selected RGBA8 bake fields on an object's process params.

        Raises:
            PipelineError: If the object's process pipeline is not an RGBA8
                bake, or it has no writable process parameters.
        """
        self._require_process_params(name, self._RGBA8_PIPELINES, "RGBA8 bake")
        name_token = self._editor.get_token(name)
        with self._editor.pipeline_params(self._token, name_token, PIPELINE_STAGE_PROCESS) as params:
            if params is None or not params.data or params.size < 20:
                raise PipelineError(f"Object {name!r} has no process parameters for RGBA8 bake")
            base = params.data
            if bake_all_directions is not None:
                cast(base, POINTER(c_int32))[0] = 1 if bake_all_directions else 0
            if ray_direction is not None:
                x, y, z = ray_direction
                floats = cast(base, POINTER(c_float))
                floats[1] = float(x)
                floats[2] = float(y)
                floats[3] = float(z)
            if upsample_factor is not None:
                cast(base, POINTER(c_uint32))[4] = max(1, min(MAX_RGBA8_UPSAMPLE, int(upsample_factor)))

    def _owns_process_params(self, name: str, expected: Tuple[str, ...]) -> bool:
        """Whether an object's process params are the struct ``expected`` describes.

        A stage's pipeline type and its parameter block are the same slot on the
        C++ side, so the type says which struct ``pipeline_params`` maps. Without
        this check the size-only guards would match a different pipeline's params
        and read or overwrite unrelated fields.
        """
        info = self.get_pipeline(name, PIPELINE_STAGE_PROCESS)
        return info is not None and info.type_id in expected

    def _require_process_params(self, name: str, expected: Tuple[str, ...], field: str) -> None:
        if not self._owns_process_params(name, expected):
            info = self.get_pipeline(name, PIPELINE_STAGE_PROCESS)
            actual = "none" if info is None else info.type_id
            raise PipelineError(f"Object {name!r} has process pipeline {actual!r}, which has no {field} parameter")

    @staticmethod
    def _clamp_voxels_per_unit(value: float) -> float:
        if not math.isfinite(value):
            return DEFAULT_VOXELS_PER_UNIT
        return max(_VOXELS_PER_UNIT_MIN, min(_VOXELS_PER_UNIT_MAX, float(value)))

    # ------------------------------------------------------------------
    # RGBA8 color-grid conversion
    # ------------------------------------------------------------------

    def nanovdb_to_rgba8(
        self,
        src: "GridInput",
        name: str = "rgba8",
        ray_direction: Sequence[float] = DEFAULT_RGBA8_RAY_DIRECTION,
        upsample_factor: int = DEFAULT_RGBA8_UPSAMPLE,
        add: bool = True,
        *,
        register: Optional[bool] = None,
    ) -> Grid:
        """Convert a VoxelBVH grid into an RGBA8 color image grid.

        Bakes per-voxel colors into a NanoVDB image grid (packed RGBA8 leaf
        values), suitable for the RGBA8 render pipeline.

        Args:
            src: A VoxelBVH NanoVDB grid (a :class:`Grid` or raw
                ``pnanovdb_ComputeArray``), e.g. the result of
                ``nanovdb_from_gaussians(process="voxelbvh")`` or
                ``nanovdb_from_mesh``.
            name: Object name to register the RGBA8 grid under.
            ray_direction: Index-space ray direction ``(x, y, z)`` used to bake
                colors (defaults to ``(0, 0, -1)``).
            upsample_factor: Topology upsampling factor, ``1``..``4``.
            add / register: When True (default), also register the grid with the
                scene. Prefer ``register=``; ``add=`` is kept as an alias.

        Returns:
            The RGBA8 grid as a :class:`~nanovdb_editor.grid.Grid`.
        """
        voxelbvh = self._editor._get_voxelbvh()
        nvdb_array = voxelbvh.nanovdb_rgba8_from_array(Grid.unwrap(src), ray_direction, upsample_factor)
        return self._finalize(nvdb_array, name, self._resolve_register(add, register))

    def nanovdb_to_rgba8_directions(
        self,
        src: "GridInput",
        name: str = "rgba8",
        directions: Optional[Sequence[Sequence[float]]] = None,
        upsample_factor: int = DEFAULT_RGBA8_UPSAMPLE,
        add: bool = True,
        *,
        register: Optional[bool] = None,
    ) -> List[Grid]:
        """Bake an RGBA8 color grid for each ray direction.

        This is the multi-direction counterpart of :meth:`nanovdb_to_rgba8`.
        Defaults to :data:`~nanovdb_editor.DEFAULT_RGBA8_DIRECTIONS` — the same
        8 directions the editor uses when ``bake_all_directions`` is enabled.

        Args:
            src: A VoxelBVH NanoVDB grid (a :class:`Grid` or raw array).
            name: Base object name; each direction is registered as
                ``"{name}_d{i}"`` when registering.
            directions: Iterable of ``(x, y, z)`` index-space ray directions.
                Defaults to :data:`~nanovdb_editor.DEFAULT_RGBA8_DIRECTIONS`.
                At most :data:`~nanovdb_editor.MAX_RGBA8_DIRECTIONS` entries.
            upsample_factor: Topology upsampling factor, ``1``..``4``.
            add / register: When True (default), also register each grid with
                the scene. Prefer ``register=``.

        Returns:
            A list of RGBA8 :class:`~nanovdb_editor.grid.Grid` objects, one per
            direction (same order as ``directions``).
        """
        directions = normalize_rgba8_directions(directions)

        do_register = self._resolve_register(add, register)
        voxelbvh = self._editor._get_voxelbvh()
        arrays = voxelbvh.nanovdb_rgba8_from_array_directions(Grid.unwrap(src), directions, upsample_factor)
        grids: List[Grid] = []
        try:
            for i, array in enumerate(arrays):
                grid_name = f"{name}_d{i}" if len(directions) > 1 else name
                grids.append(self._finalize(array, grid_name, do_register))
        except Exception:
            for grid in grids:
                try:
                    grid.close()
                except Exception:
                    pass
            # Any arrays not yet wrapped still need cleanup.
            for array in arrays[len(grids) :]:
                try:
                    self._editor._compute.destroy_array(array)
                except Exception:
                    pass
            raise
        return grids

    def nanovdb_from_gaussians(
        self,
        means,
        quats,
        scales,
        sh_0,
        sh_n=None,
        opacities=None,
        process="raster3d",
        voxel_size: float = 1.0 / 128.0,
        resolution: int = 512,
        name: str = "gaussians",
        add: bool = True,
        *,
        register: Optional[bool] = None,
    ) -> Grid:
        """Build a NanoVDB grid from raw Gaussian splat arrays.

        Args:
            means: (N, 3) positions.
            quats: (N, 4) rotation quaternions (w, x, y, z).
            scales: (N, 3) log-space scales.
            sh_0: (N, 3) order-0 spherical-harmonic coefficients (base color).
            sh_n: higher-order spherical-harmonic coefficients, or ``None``.
            opacities: (N,) logit-space opacities.
            process: ``"raster3d"`` / ``"voxelbvh"``, a matching
                :class:`PipelineType`, or :class:`PipelineInfo`.
            voxel_size: World-space size of a voxel (``"raster3d"`` only).
            resolution: VoxelBVH grid resolution, 1..4096 (``"voxelbvh"`` only).
            name: Object name to register the grid under within the scene.
            add / register: When True (default), also register the grid with the
                scene. Prefer ``register=``.

        Returns:
            The resulting grid as a :class:`~nanovdb_editor.grid.Grid`.
        """
        process = self._resolve_gaussian_process(process)
        if opacities is None:
            raise InvalidArgumentError("opacities is required")

        with self._Arrays(self._editor._compute) as arrays:
            if sh_n is None:
                # Native code derives the SH stride from sh_n's element count, so an
                # empty buffer means "no higher-order SH". A zero-filled array shaped
                # like sh_0 would instead imply a stride of 1.
                sh_n = np.zeros(0, dtype=np.float32)

            # Canonical Gaussian array order shared by both pipelines.
            gaussian_arrays = [
                arrays.to_array(means),
                arrays.to_array(opacities),
                arrays.to_array(quats),
                arrays.to_array(scales),
                arrays.to_array(sh_0),
                arrays.to_array(sh_n),
            ]

            if process == "raster3d":
                raster = self._editor._get_raster()
                nvdb_array = raster.raster_to_nanovdb_from_arrays(voxel_size, gaussian_arrays)
            else:  # "voxelbvh"
                voxelbvh = self._editor._get_voxelbvh()
                nvdb_array = voxelbvh.nanovdb_from_gaussians_array(gaussian_arrays, resolution)

        return self._finalize(nvdb_array, name, self._resolve_register(add, register))

    def nanovdb_from_mesh(
        self,
        indices,
        positions,
        colors=None,
        process="voxelbvh",
        inflation_radius: float = 0.0,
        resolution: int = 512,
        name: str = "mesh",
        add: bool = True,
        *,
        register: Optional[bool] = None,
    ) -> Grid:
        """Build a NanoVDB grid from a triangle mesh via the VoxelBVH pipeline.

        Args:
            indices: Flat ``uint32`` triangle indices (3 per triangle).
            positions: (V, 3) or flat ``float32`` vertex positions.
            colors: Optional (V, 3) per-vertex RGB; defaults to white.
            process: Conversion pipeline; currently ``"voxelbvh"`` (or matching
                :class:`PipelineType` / :class:`PipelineInfo`).
            inflation_radius: World-space inflation of each triangle.
            resolution: VoxelBVH grid resolution (1..4096).
            name: Object name to register the grid under within the scene.
            add / register: When True (default), also register the grid with the
                scene. Prefer ``register=``.

        Returns:
            The resulting grid as a :class:`~nanovdb_editor.grid.Grid`.
        """
        return self._nanovdb_from_primitives(
            "triangles",
            indices,
            positions,
            colors,
            process,
            inflation_radius,
            resolution,
            name,
            self._resolve_register(add, register),
        )

    def nanovdb_from_lines(
        self,
        indices,
        positions,
        colors=None,
        process="voxelbvh",
        inflation_radius: float = 0.0,
        resolution: int = 512,
        name: str = "lines",
        add: bool = True,
        *,
        register: Optional[bool] = None,
    ) -> Grid:
        """Build a NanoVDB grid from a line set via the VoxelBVH pipeline.

        Args:
            indices: Flat ``uint32`` line indices (2 per segment).
            positions: (V, 3) or flat ``float32`` vertex positions.
            colors: Optional (V, 3) per-vertex RGB; defaults to white.
            process: Conversion pipeline; currently ``"voxelbvh"`` (or matching
                :class:`PipelineType` / :class:`PipelineInfo`).
            inflation_radius: World-space inflation of each line.
            resolution: VoxelBVH grid resolution (1..4096).
            name: Object name to register the grid under within the scene.
            add / register: When True (default), also register the grid with the
                scene. Prefer ``register=``.

        Returns:
            The resulting grid as a :class:`~nanovdb_editor.grid.Grid`.
        """
        return self._nanovdb_from_primitives(
            "lines",
            indices,
            positions,
            colors,
            process,
            inflation_radius,
            resolution,
            name,
            self._resolve_register(add, register),
        )

    def _nanovdb_from_primitives(
        self, kind, indices, positions, colors, process, inflation_radius, resolution, name, add
    ):
        if isinstance(process, PipelineInfo):
            process = process.process or process.type_id
        if process in (PipelineType.voxelbvh_build, "voxelbvh_build"):
            process = "voxelbvh"
        if process != "voxelbvh":
            raise InvalidArgumentError(f"Unsupported process {process!r} for {kind}; supported: ('voxelbvh',)")

        with self._Arrays(self._editor._compute) as arrays:
            if colors is None:
                colors = np.ones(self._position_float_count(positions), dtype=np.float32)

            indices_array = arrays.to_array(indices, dtype=np.uint32)
            positions_array = arrays.to_array(positions)
            colors_array = arrays.to_array(colors)

            voxelbvh = self._editor._get_voxelbvh()
            if kind == "triangles":
                nvdb_array = voxelbvh.nanovdb_from_triangles_array(
                    indices_array, positions_array, colors_array, inflation_radius, resolution
                )
            else:  # "lines"
                nvdb_array = voxelbvh.nanovdb_from_lines_array(
                    indices_array, positions_array, colors_array, inflation_radius, resolution
                )

        return self._finalize(nvdb_array, name, add)
