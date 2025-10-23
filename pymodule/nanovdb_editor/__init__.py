# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import sys

from .compiler import Compiler, CompileTarget, MemoryBuffer
from .compute import Compute
from .device import DeviceInterface
from .editor import (
    Editor,
    EditorConfig,
    pnanovdb_EditorToken as EditorToken,
    pnanovdb_EditorGaussianDataDesc as EditorGaussianDataDesc,
    pnanovdb_Camera as Camera,
    pnanovdb_CameraView as CameraView,
    pnanovdb_CameraConfig as CameraConfig,
    pnanovdb_CameraState as CameraState,
    pnanovdb_Vec3 as Vec3,
)
from .raster import Raster

if sys.platform == "win32":
    lib_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
    if os.path.exists(lib_dir):
        from .utils import add_dll_search_directory

        add_dll_search_directory(lib_dir)


__all__ = [
    "Compiler",
    "Compute",
    "DeviceInterface",
    "Editor",
    "Raster",
    "CompileTarget",
    "MemoryBuffer",
    "EditorConfig",
    "EditorToken",
    "EditorGaussianDataDesc",
    "Camera",
    "CameraView",
    "CameraConfig",
    "CameraState",
    "Vec3",
]
