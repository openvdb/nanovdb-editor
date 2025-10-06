# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from ctypes import (
    Structure,
    POINTER,
    CFUNCTYPE,
    c_void_p,
    c_char_p,
    c_int,
    c_int32,
    c_uint32,
    c_float,
    byref,
    pointer,
    cast,
)

from .compute import Compute, pnanovdb_Compute, pnanovdb_ComputeArray
from .compiler import Compiler, pnanovdb_Compiler
from .device import pnanovdb_Device
from .utils import load_library

EDITOR_LIB = "pnanovdbeditor"


# Match pnanovdb_bool_t (int32_t)
pnanovdb_bool_t = c_int32


class pnanovdb_EditorConfig(Structure):
    """Definition equivalent to pnanovdb_editor_config_t."""

    _fields_ = [
        ("ip_address", c_char_p),
        ("port", c_int32),
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
        ("is_projection_rh", pnanovdb_bool_t),
        ("is_orthographic", pnanovdb_bool_t),
        ("is_reverse_z", pnanovdb_bool_t),
        ("near_plane", c_float),
        ("far_plane", c_float),
        ("fov_angle_y", c_float),
        ("orthographic_y", c_float),
        ("aspect_ratio", c_float),
        ("pan_rate", c_float),
        ("tilt_rate", c_float),
        ("zoom_rate", c_float),
        ("key_translation_rate", c_float),
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
        ("mouse_x_prev", c_int),
        ("mouse_y_prev", c_int),
        ("rotation_active", pnanovdb_bool_t),
        ("zoom_active", pnanovdb_bool_t),
        ("translate_active", pnanovdb_bool_t),
        ("key_translate_active_mask", c_uint32),
    ]


class pnanovdb_CameraView(Structure):
    """Definition equivalent to pnanovdb_camera_view_t."""

    _fields_ = [
        ("name", c_char_p),
        ("configs", POINTER(pnanovdb_CameraConfig)),
        ("states", POINTER(pnanovdb_CameraState)),
        ("num_cameras", c_uint32),
        ("axis_length", c_float),
        ("axis_thickness", c_float),
        ("axis_scale", c_float),
        ("frustum_line_width", c_float),
        ("frustum_scale", c_float),
        ("frustum_color", pnanovdb_Vec3),
        ("is_visible", pnanovdb_bool_t),
    ]


class pnanovdb_Editor(Structure):
    """Definition equivalent to pnanovdb_editor_t."""

    _fields_ = [
        ("interface_pnanovdb_reflect_data_type", c_void_p),
        ("module", c_void_p),
        ("impl", c_void_p),
        ("init", CFUNCTYPE(None, c_void_p)),
        (
            "init_impl",
            CFUNCTYPE(
                c_int32,  # pnanovdb_bool_t
                c_void_p,  # pnanovdb_editor_t*
                POINTER(pnanovdb_Compute),  # const pnanovdb_compute_t*
                POINTER(pnanovdb_Compiler),  # const pnanovdb_compiler_t*
            ),
        ),
        ("shutdown", CFUNCTYPE(None, c_void_p)),
        (
            "show",
            CFUNCTYPE(
                None,
                c_void_p,
                POINTER(pnanovdb_Device),
                POINTER(pnanovdb_EditorConfig),
            ),
        ),
        (
            "start",
            CFUNCTYPE(
                None,
                c_void_p,
                POINTER(pnanovdb_Device),
                POINTER(pnanovdb_EditorConfig),
            ),
        ),
        ("stop", CFUNCTYPE(None, c_void_p)),
        (
            "add_nanovdb",
            CFUNCTYPE(None, c_void_p, POINTER(pnanovdb_ComputeArray)),
        ),
        (
            "add_array",
            CFUNCTYPE(None, c_void_p, POINTER(pnanovdb_ComputeArray)),
        ),
        (
            "add_gaussian_data",
            CFUNCTYPE(None, c_void_p, c_void_p, c_void_p, c_void_p),
        ),  # raster, queue, gaussian
        ("update_camera", CFUNCTYPE(None, c_void_p, POINTER(pnanovdb_Camera))),
        (
            "add_camera_view",
            CFUNCTYPE(None, c_void_p, POINTER(pnanovdb_CameraView)),
        ),
        ("add_shader_params", CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)),
        # params, data_type
        (
            "sync_shader_params",
            CFUNCTYPE(
                None,
                c_void_p,
                c_void_p,
                c_int32,
            ),
        ),
    ]


class pnanovdb_EditorImpl(Structure):
    """Mirror of pnanovdb_editor_impl_t for read-only access.

    Access is for reading only and structure must match C++ layout.
    """

    _fields_ = [
        ("compiler", POINTER(pnanovdb_Compiler)),
        ("compute", POINTER(pnanovdb_Compute)),
        ("editor_worker", c_void_p),
        ("nanovdb_array", POINTER(pnanovdb_ComputeArray)),
        ("data_array", POINTER(pnanovdb_ComputeArray)),
        ("gaussian_data", c_void_p),
        ("camera", POINTER(pnanovdb_Camera)),
        ("raster_ctx", c_void_p),
        ("shader_params", c_void_p),
        ("shader_params_data_type", c_void_p),
        ("loaded", c_void_p),
        ("views", c_void_p),
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

        # Assign module handle for editor; mirror pnanovdb_editor_load
        self._editor.contents.module = self._lib._handle

        init_impl = getattr(self._editor.contents, "init_impl", None)
        result = init_impl(
            self._editor,
            compute.get_compute(),
            compiler.get_compiler(),
        )
        if result == 0:
            self._editor.contents.init(self._editor)

    def shutdown(self) -> None:
        shutdown_func = self._editor.contents.shutdown
        shutdown_func(self._editor)

    def update_camera(self, camera: pnanovdb_Camera) -> None:
        udpate_camera_func = self._editor.contents.update_camera
        udpate_camera_func(self._editor, pointer(camera))

    def add_nanovdb(self, array: pnanovdb_ComputeArray) -> None:
        add_nanovdb_func = self._editor.contents.add_nanovdb
        add_nanovdb_func(self._editor, pointer(array))

    def add_array(self, array: pnanovdb_ComputeArray) -> None:
        add_array_func = self._editor.contents.add_array
        add_array_func(self._editor, pointer(array))

    def add_gaussian_data(self, raster, queue, data) -> None:
        """Add gaussian data to the editor."""
        add_gaussian_data_func = self._editor.contents.add_gaussian_data
        add_gaussian_data_func(self._editor, raster, queue, data)

    def add_shader_params(self, params, data_type) -> None:
        """Setup shader parameters."""
        add_shader_params_func = self._editor.contents.add_shader_params
        add_shader_params_func(self._editor, params, data_type)

    def sync_shader_params(self, params, set_data: bool) -> None:
        """Sync shader parameters with editor thread.

        params should be a pointer to the same structure previously provided
        to add_gaussian_data/add_shader_params.
        """
        sync_shader_params_func = self._editor.contents.sync_shader_params
        sync_shader_params_func(self._editor, params, 1 if set_data else 0)

    def show(self, config=None) -> None:
        show_func = self._editor.contents.show

        try:
            if config is None:
                config = pnanovdb_EditorConfig(b"127.0.0.1", 8080, 0, 0)
            show_func(
                self._editor,
                self._compute.device_interface().get_device(),
                byref(config),
            )
        except (OSError, ValueError, RuntimeError) as e:
            print(f"Error: Editor runtime error ({e})")

    def start(self, config=None) -> None:
        """Start the editor."""
        start_func = self._editor.contents.start

        try:
            if config is None:
                config = pnanovdb_EditorConfig(b"127.0.0.1", 8080, 0, 0)
            start_func(
                self._editor,
                self._compute.device_interface().get_device(),
                byref(config),
            )
        except (OSError, ValueError, RuntimeError) as e:
            print(f"Error: Editor start error ({e})")

    def stop(self) -> None:
        """Stop the editor."""
        stop_func = self._editor.contents.stop
        stop_func(self._editor)

    def get_nanovdb(self) -> pnanovdb_ComputeArray:
        impl_ptr = cast(
            self._editor.contents.impl,
            POINTER(pnanovdb_EditorImpl),
        )
        if not impl_ptr or not impl_ptr.contents.nanovdb_array:
            raise RuntimeError("No NanoVDB array available")
        return impl_ptr.contents.nanovdb_array.contents

    def get_array(self) -> pnanovdb_ComputeArray:
        impl_ptr = cast(
            self._editor.contents.impl,
            POINTER(pnanovdb_EditorImpl),
        )
        if not impl_ptr or not impl_ptr.contents.data_array:
            raise RuntimeError("No data array available")
        return impl_ptr.contents.data_array.contents

    def add_callable(self, name: str, func) -> None:
        """Compatibility stub for older API; no-op in current interface."""
        _ = (name, func)

    def __del__(self):
        self._editor = None
