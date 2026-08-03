# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""High-level Python interface to the editor's registered pipelines.

The C++ editor registers a fixed set of pipelines (see ``editor/PipelineTypes.h``)
grouped into three stages: ``load``, ``process`` and ``render``. Render pipelines
only affect how an object is drawn inside the editor and can be assigned via
:meth:`nanovdb_editor.scene.Scene.set_render_pipeline` (or the lower-level
:meth:`nanovdb_editor.editor.Editor.set_pipeline`). The ``process`` pipelines are
the ones that actually build a NanoVDB grid from higher-level inputs, and those
are what this module exposes to Python:

- ``gaussian_voxelize`` (``"raster3d"``) -> :meth:`Raster.raster_to_nanovdb_from_arrays`
- ``voxelbvh_build`` (``"voxelbvh"``) from Gaussians, triangle meshes or lines
  -> :class:`~nanovdb_editor.voxelbvh.VoxelBVH`

The :class:`~nanovdb_editor.editor.Scene` helpers (``nanovdb_from_gaussians``,
``nanovdb_from_mesh``, ``nanovdb_from_lines``) dispatch to these based on the
``process`` argument.
"""

from enum import IntEnum

# VoxelBVH lives in its own module; re-exported here for convenience so that
# ``from nanovdb_editor.pipelines import VoxelBVH`` keeps working.
from .voxelbvh import (  # noqa: F401
    VoxelBVH,
    pnanovdb_Vec3,
    pnanovdb_VoxelBVH,
    DEFAULT_BVH_RESOLUTION,
    MAX_BVH_RESOLUTION,
    DEFAULT_RGBA8_UPSAMPLE,
    MAX_RGBA8_UPSAMPLE,
    DEFAULT_RGBA8_RAY_DIRECTION,
    DEFAULT_RGBA8_DIRECTIONS,
    PNANOVDB_GRID_TYPE_RGBA8,
)

# Defaults mirrored from editor/PipelineTypes.h (pnanovdb_editor namespace).
DEFAULT_VOXELS_PER_UNIT = 128.0
DEFAULT_VOXEL_SIZE = 1.0 / DEFAULT_VOXELS_PER_UNIT


class PipelineStage(IntEnum):
    """Pipeline stages (``pnanovdb_pipeline_stage_t``).

    Members are ``int`` subclasses, so they can be passed anywhere the C stage
    value is expected while still offering ``.name`` and a readable ``repr``.
    """

    LOAD = 0
    PROCESS = 1
    RENDER = 2


# Backward-compatible module-level aliases.
PIPELINE_STAGE_LOAD = PipelineStage.LOAD
PIPELINE_STAGE_PROCESS = PipelineStage.PROCESS
PIPELINE_STAGE_RENDER = PipelineStage.RENDER

_STAGE_NAMES = {
    PipelineStage.LOAD: "load",
    PipelineStage.PROCESS: "process",
    PipelineStage.RENDER: "render",
}


class PipelineInfo:
    """Static description of a registered pipeline type.

    Mirrors an entry of ``pnanovdb_pipeline_type_enum_t`` so pipelines can be
    introspected from Python without a running editor.
    """

    __slots__ = ("value", "type_id", "stage", "description", "process")

    def __init__(self, value, type_id, stage, description, process=None):
        self.value = value
        #: Enum name without the ``pnanovdb_pipeline_type_`` prefix.
        self.type_id = type_id
        self.stage = stage
        self.description = description
        #: Friendly ``process=`` alias accepted by the Scene helpers, if any.
        self.process = process

    @property
    def stage_name(self):
        return _STAGE_NAMES.get(self.stage, "unknown")

    @property
    def enum_name(self):
        """Full C enum name, e.g. ``pnanovdb_pipeline_type_gaussian_voxelize``."""
        return f"pnanovdb_pipeline_type_{self.type_id}"

    @property
    def type(self):
        """This pipeline as a :class:`PipelineType` enum member."""
        return PipelineType(self.value)

    @property
    def stage_enum(self):
        """This pipeline's stage as a :class:`PipelineStage` enum member."""
        return PipelineStage(self.stage)

    def __int__(self):
        return self.value

    def __index__(self):
        return self.value

    def __repr__(self):
        return (
            f"PipelineInfo(value={self.value}, type_id={self.type_id!r}, "
            f"stage={self.stage_name!r}, process={self.process!r})"
        )


# Registry mirroring editor/PipelineTypes.h. Keep values in sync with the C enum.
PIPELINE_REGISTRY = (
    PipelineInfo(0, "noop", PIPELINE_STAGE_LOAD, "no-op load stage"),
    PipelineInfo(1, "nanovdb_render", PIPELINE_STAGE_RENDER, "ray-march a NanoVDB grid"),
    PipelineInfo(2, "gaussian_splat", PIPELINE_STAGE_RENDER, "2D Gaussian splatting"),
    PipelineInfo(3, "gaussian_voxelize", PIPELINE_STAGE_PROCESS, "Gaussians to NanoVDB", process="raster3d"),
    PipelineInfo(4, "voxelbvh_gaussians_render", PIPELINE_STAGE_RENDER, "VoxelBVH built from gaussians"),
    PipelineInfo(5, "voxelbvh_lines_render", PIPELINE_STAGE_RENDER, "VoxelBVH as lines"),
    PipelineInfo(6, "voxelbvh_triangles_render", PIPELINE_STAGE_RENDER, "VoxelBVH as triangles"),
    PipelineInfo(7, "voxelbvh_triangles_debug_render", PIPELINE_STAGE_RENDER, "triangles, debug shading"),
    PipelineInfo(8, "voxelbvh_debug_render", PIPELINE_STAGE_RENDER, "VoxelBVH, debug shading"),
    PipelineInfo(
        9, "voxelbvh_build", PIPELINE_STAGE_PROCESS, "build a VoxelBVH (from mesh/gaussians)", process="voxelbvh"
    ),
    PipelineInfo(10, "mesh_load", PIPELINE_STAGE_LOAD, "read a PLY into compute arrays"),
    PipelineInfo(11, "gaussian_load", PIPELINE_STAGE_LOAD, "import a Gaussian file into gaussian_data"),
    PipelineInfo(12, "nanovdb_surface", PIPELINE_STAGE_RENDER, "SDF/level-set isosurface via HDDA zero-crossing"),
    PipelineInfo(13, "image2d_render", PIPELINE_STAGE_RENDER, "NanoVDB image grid (RGBA) to a 2D texture"),
    PipelineInfo(14, "voxelbvh_rgba8", PIPELINE_STAGE_PROCESS, "VoxelBVH to RGBA8 NanoVDB"),
    PipelineInfo(15, "voxelbvh_rgba8_chain", PIPELINE_STAGE_PROCESS, "VoxelBVH build then RGBA8 conversion"),
    PipelineInfo(16, "voxelbvh_rgba8_render", PIPELINE_STAGE_RENDER, "RGBA8 NanoVDB with directional grid selection"),
    PipelineInfo(17, "nanovdb_load", PIPELINE_STAGE_LOAD, "load a NanoVDB grid"),
)

_REGISTRY_BY_TYPE_ID = {info.type_id: info for info in PIPELINE_REGISTRY}
_REGISTRY_BY_VALUE = {info.value: info for info in PIPELINE_REGISTRY}
_REGISTRY_BY_PROCESS = {info.process: info for info in PIPELINE_REGISTRY if info.process}


# Enum of every registered pipeline type, generated from the registry so it
# stays in sync. Members are named by ``type_id`` (e.g. ``PipelineType.voxelbvh_build``).
PipelineType = IntEnum(
    "PipelineType",
    {info.type_id: info.value for info in PIPELINE_REGISTRY},
)
PipelineType.__doc__ = "Registered pipeline types (``pnanovdb_pipeline_type_t``)."


def list_pipelines(stage=None):
    """Return the registered pipelines, optionally filtered by stage.

    Args:
        stage: One of ``PIPELINE_STAGE_LOAD``/``PROCESS``/``RENDER`` or the
            matching name string (``"load"``, ``"process"``, ``"render"``).
    """
    if stage is None:
        return list(PIPELINE_REGISTRY)
    if isinstance(stage, str):
        stage = {v: k for k, v in _STAGE_NAMES.items()}.get(stage)
    return [info for info in PIPELINE_REGISTRY if info.stage == stage]


def get_pipeline_info(name):
    """Look up a pipeline by ``type_id``, enum value, or ``process`` alias."""
    if isinstance(name, PipelineInfo):
        return name
    if isinstance(name, int):
        return _REGISTRY_BY_VALUE.get(int(name))
    if name in _REGISTRY_BY_PROCESS:
        return _REGISTRY_BY_PROCESS[name]
    if name in _REGISTRY_BY_TYPE_ID:
        return _REGISTRY_BY_TYPE_ID[name]
    # Accept the fully-qualified enum name too.
    prefix = "pnanovdb_pipeline_type_"
    if isinstance(name, str) and name.startswith(prefix):
        return _REGISTRY_BY_TYPE_ID.get(name[len(prefix) :])
    return None
