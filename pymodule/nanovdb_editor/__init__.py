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
    EditorToken,
    EditorGaussianDataDesc,
    Camera,
    CameraView,
    CameraConfig,
    CameraState,
    Vec3,
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
