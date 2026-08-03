# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import sys

if sys.platform == "win32":
    lib_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
    if os.path.exists(lib_dir):
        from .utils import add_dll_search_directory

        add_dll_search_directory(lib_dir)

from .compiler import Compiler, CompileTarget, MemoryBuffer, OptimizationLevel
from .compute import Array, Compute
from .device import DeviceInterface
from .editor import (
    Editor,
    EditorConfig,
    EditorToken,
    EditorGaussianDataDesc,
    make_editor_config,
    Camera,
    CameraView,
    CameraConfig,
    CameraState,
    Vec3,
)
from .scene import Scene, ProcessChain, ProcessStep
from .grid import Grid
from .raster import Raster
from .voxelbvh import VoxelBVH
from .exceptions import (
    NanoVDBError,
    DeviceError,
    PipelineError,
    InvalidArgumentError,
    SessionClosedError,
)
from .pipelines import (
    PipelineInfo,
    PipelineStage,
    PipelineType,
    PIPELINE_REGISTRY,
    PIPELINE_STAGE_LOAD,
    PIPELINE_STAGE_PROCESS,
    PIPELINE_STAGE_RENDER,
    DEFAULT_VOXEL_SIZE,
    DEFAULT_VOXELS_PER_UNIT,
    DEFAULT_BVH_RESOLUTION,
    MAX_BVH_RESOLUTION,
    DEFAULT_RGBA8_UPSAMPLE,
    MAX_RGBA8_UPSAMPLE,
    DEFAULT_RGBA8_RAY_DIRECTION,
    DEFAULT_RGBA8_DIRECTIONS,
    PNANOVDB_GRID_TYPE_RGBA8,
    list_pipelines,
    get_pipeline_info,
)
from .session import Session, create_default

__all__ = [
    "Compiler",
    "Compute",
    "Array",
    "DeviceInterface",
    "Editor",
    "Scene",
    "ProcessChain",
    "ProcessStep",
    "Grid",
    "Raster",
    "VoxelBVH",
    "NanoVDBError",
    "DeviceError",
    "PipelineError",
    "InvalidArgumentError",
    "SessionClosedError",
    "PipelineInfo",
    "PipelineStage",
    "PipelineType",
    "PIPELINE_REGISTRY",
    "PIPELINE_STAGE_LOAD",
    "PIPELINE_STAGE_PROCESS",
    "PIPELINE_STAGE_RENDER",
    "DEFAULT_VOXEL_SIZE",
    "DEFAULT_VOXELS_PER_UNIT",
    "DEFAULT_BVH_RESOLUTION",
    "MAX_BVH_RESOLUTION",
    "DEFAULT_RGBA8_UPSAMPLE",
    "MAX_RGBA8_UPSAMPLE",
    "DEFAULT_RGBA8_RAY_DIRECTION",
    "DEFAULT_RGBA8_DIRECTIONS",
    "PNANOVDB_GRID_TYPE_RGBA8",
    "list_pipelines",
    "get_pipeline_info",
    "Session",
    "CompileTarget",
    "MemoryBuffer",
    "OptimizationLevel",
    "EditorConfig",
    "make_editor_config",
    "EditorToken",
    "EditorGaussianDataDesc",
    "Camera",
    "CameraView",
    "CameraConfig",
    "CameraState",
    "Vec3",
    "create_default",
]
