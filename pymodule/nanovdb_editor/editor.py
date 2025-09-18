# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from ctypes import *

from .compute import Compute, pnanovdb_Compute, pnanovdb_ComputeArray
from .compiler import Compiler, pnanovdb_Compiler
from .device import pnanovdb_Device
from .utils import load_library

EDITOR_LIB = "pnanovdbeditor"


class pnanovdb_EditorConfig(Structure):
    """Definition equivalent to pnanovdb_editor_config_t."""
    _fields_ = [
        ("ip_address", c_char_p),
        ("port", c_int),
        ("headless", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("streaming", c_int32),  # pnanovdb_bool_t is int32_t in C
    ]


class pnanovdb_Vec3(Structure):
    """Definition equivalent to pnanovdb_vec3_t."""
    _fields_ = [
        ("x", c_float),
        ("y", c_float),
        ("z", c_float),
    ]


class pnanovdb_CameraConfig(Structure):
    """Definition equivalent to pnanovdb_camera_config_t."""
    _fields_ = [
        ("position", pnanovdb_Vec3),
        ("eye_direction", pnanovdb_Vec3),
        ("eye_up", pnanovdb_Vec3),
        ("eye_distance_from_position", c_float),
        ("orthographic_scale", c_float),
    ]


class pnanovdb_CameraState(Structure):
    """Definition equivalent to pnanovdb_camera_state_t."""
    _fields_ = [
        ("position", pnanovdb_Vec3),
        ("eye_direction", pnanovdb_Vec3),
        ("eye_up", pnanovdb_Vec3),
        ("eye_distance_from_position", c_float),
        ("orthographic_scale", c_float),
    ]


class pnanovdb_Camera(Structure):
    """Definition equivalent to pnanovdb_camera_t."""
    _fields_ = [
        ("config", pnanovdb_CameraConfig),
        ("state", pnanovdb_CameraState),
    ]


class pnanovdb_Editor(Structure):
    """Definition equivalent to pnanovdb_editor_t."""
    _fields_ = [
        ("interface_pnanovdb_reflect_data_type", c_void_p),  # PNANOVDB_REFLECT_INTERFACE()
        ("compiler", POINTER(pnanovdb_Compiler)),
        ("compute", POINTER(pnanovdb_Compute)),
        ("init", CFUNCTYPE(None, c_void_p)),
        ("shutdown", CFUNCTYPE(None, c_void_p)),
        ("add_nanovdb", CFUNCTYPE(None,
                                 c_void_p,
                                 POINTER(pnanovdb_ComputeArray))),
        ("add_array", CFUNCTYPE(None,
                               c_void_p,
                               POINTER(pnanovdb_ComputeArray))),
        ("add_camera", CFUNCTYPE(None, c_void_p, POINTER(pnanovdb_Camera))),
        ("show", CFUNCTYPE(None, c_void_p, POINTER(pnanovdb_Device),
                          POINTER(pnanovdb_EditorConfig))),
        ("module", c_void_p),
        ("gaussian_data", c_void_p),  # pnanovdb_raster_gaussian_data_t*
        ("nanovdb_array", POINTER(pnanovdb_ComputeArray)),
        ("data_array", POINTER(pnanovdb_ComputeArray)),
        ("camera", POINTER(pnanovdb_Camera)),
    ]


class Editor:
    """Python wrapper for pnanovdb_editor_t."""
    def __init__(self, compute: Compute, compiler: Compiler):
        self._lib = load_library(EDITOR_LIB)

        get_editor = self._lib.pnanovdb_get_editor
        get_editor.restype = POINTER(pnanovdb_Editor)
        get_editor.argtypes = []

        self._editor = get_editor()
        if not self._editor:
            raise RuntimeError("Failed to get editor interface")

        self._compute = compute
        self._compiler = compiler

        # Assign the compute and compiler pointers first
        compute_ptr = compute.get_compute()
        self._editor.contents.compute = compute_ptr
        self._editor.contents.compiler = compiler.get_compiler()
        self._editor.contents.module = self._lib._handle
        self._editor.contents.compute.contents.module = compute._lib._handle

        init_func = self._editor.contents.init
        init_func(self._editor)

    def shutdown(self) -> None:
        shutdown_func = self._editor.contents.shutdown
        shutdown_func(self._editor)

    def add_camera(self, camera: pnanovdb_Camera) -> None:
        add_camera_func = self._editor.contents.add_camera
        add_camera_func(self._editor, pointer(camera))

    def add_nanovdb(self, array: pnanovdb_ComputeArray) -> None:
        add_nanovdb_func = self._editor.contents.add_nanovdb
        add_nanovdb_func(self._editor, pointer(array))

    def add_array(self, array: pnanovdb_ComputeArray) -> None:
        add_array_func = self._editor.contents.add_array
        add_array_func(self._editor, pointer(array))

    def show(self, config=None) -> None:
        show_func = self._editor.contents.show

        try:
            if config is None:
                # Create default config
                config = pnanovdb_EditorConfig()
                config.ip_address = b"127.0.0.1"
                config.port = 8080
                config.headless = 0  # pnanovdb_bool_t
                config.streaming = 0  # pnanovdb_bool_t
            show_func(self._editor,
                     self._compute.device_interface().get_device(),
                     byref(config))
        except Exception as e:
            print(f"Error: Editor runtime error ({e})")

    def get_nanovdb(self) -> pnanovdb_ComputeArray:
        return self._editor.contents.nanovdb_array.contents

    def get_array(self) -> pnanovdb_ComputeArray:
        return self._editor.contents.data_array.contents

    def __del__(self):
        if self._editor:
            self.shutdown()

        self._editor = None
