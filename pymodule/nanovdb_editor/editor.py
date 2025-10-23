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
    c_uint64,
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


class EditorToken(Structure):
    """Definition equivalent to pnanovdb_editor_token_t."""

    _fields_ = [
        ("id", c_uint64),
        ("str", c_char_p),
    ]


class EditorConfig(Structure):
    """Definition equivalent to pnanovdb_editor_config_t."""

    _fields_ = [
        ("ip_address", c_char_p),
        ("port", c_int32),
        ("headless", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("streaming", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("stream_to_file", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("ui_profile_name", c_char_p),
        ("device_index", c_int32),
    ]


class Vec3(Structure):
    """Definition equivalent to pnanovdb_vec3_t."""

    _fields_ = [
        ("x", c_float),
        ("y", c_float),
        ("z", c_float),
    ]


class CameraConfig(Structure):
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
        ("scroll_zoom_rate", c_float),
    ]


class CameraState(Structure):
    """Definition equivalent to pnanovdb_camera_state_t."""

    _fields_ = [
        ("position", Vec3),
        ("eye_direction", Vec3),
        ("eye_up", Vec3),
        ("eye_distance_from_position", c_float),
        ("orthographic_scale", c_float),
    ]


class Camera(Structure):
    """Definition equivalent to pnanovdb_camera_t."""

    _fields_ = [
        ("config", CameraConfig),
        ("state", CameraState),
        ("mouse_x_prev", c_int),
        ("mouse_y_prev", c_int),
        ("rotation_active", pnanovdb_bool_t),
        ("zoom_active", pnanovdb_bool_t),
        ("translate_active", pnanovdb_bool_t),
        ("key_translate_active_mask", c_uint32),
    ]


class CameraView(Structure):
    """Definition equivalent to pnanovdb_camera_view_t."""

    _fields_ = [
        ("name", POINTER(EditorToken)),
        ("configs", POINTER(CameraConfig)),
        ("states", POINTER(CameraState)),
        ("num_cameras", c_uint32),
        ("axis_length", c_float),
        ("axis_thickness", c_float),
        ("frustum_line_width", c_float),
        ("frustum_scale", c_float),
        ("frustum_color", Vec3),
        ("is_visible", pnanovdb_bool_t),
    ]


class EditorGaussianDataDesc(Structure):
    """Definition equivalent to pnanovdb_editor_gaussian_data_desc_t."""

    _fields_ = [
        ("means", POINTER(pnanovdb_ComputeArray)),
        ("opacities", POINTER(pnanovdb_ComputeArray)),
        ("quaternions", POINTER(pnanovdb_ComputeArray)),
        ("scales", POINTER(pnanovdb_ComputeArray)),
        ("sh_0", POINTER(pnanovdb_ComputeArray)),
        ("sh_n", POINTER(pnanovdb_ComputeArray)),
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
                POINTER(EditorConfig),
            ),
        ),
        (
            "start",
            CFUNCTYPE(
                None,
                c_void_p,
                POINTER(pnanovdb_Device),
                POINTER(EditorConfig),
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
        ("update_camera", CFUNCTYPE(None, c_void_p, POINTER(Camera))),
        (
            "add_camera_view",
            CFUNCTYPE(None, c_void_p, POINTER(CameraView)),
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
        (
            "get_resolved_port",
            CFUNCTYPE(c_int32, c_void_p, c_int32),
        ),
        # Token-based API functions
        (
            "get_camera",
            CFUNCTYPE(
                POINTER(Camera),
                c_void_p,
                POINTER(EditorToken),
            ),
        ),
        (
            "get_token",
            CFUNCTYPE(POINTER(EditorToken), c_char_p),
        ),
        (
            "add_nanovdb_2",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                POINTER(pnanovdb_ComputeArray),  # array
            ),
        ),
        (
            "add_gaussian_data_2",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                POINTER(EditorGaussianDataDesc),  # desc
            ),
        ),
        (
            "add_camera_view_2",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(CameraView),  # camera
            ),
        ),
        (
            "update_camera_2",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(Camera),  # camera
            ),
        ),
        (
            "remove",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
            ),
        ),
        (
            "map_params",
            CFUNCTYPE(
                c_void_p,  # returns void*
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_void_p,  # const pnanovdb_reflect_data_type_t*
            ),
        ),
        (
            "unmap_params",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
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
        ("camera", POINTER(Camera)),
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
        if result != 0:
            self._editor.contents.init(self._editor)

    def _get_or_default_config(
        self, config: EditorConfig | None
    ) -> EditorConfig:
        if config is not None:
            return config
        cfg = EditorConfig()
        cfg.ip_address = b"127.0.0.1"
        cfg.port = 8080
        cfg.headless = 0
        cfg.streaming = 0
        cfg.stream_to_file = 0
        return cfg

    def _ensure_device(self, config: EditorConfig) -> None:
        di = self._compute.device_interface()
        has_device = False
        try:
            _ = di.get_device()
            has_device = True
        except RuntimeError:
            has_device = False
        if not has_device:
            di.create_device_manager(False)
            di.create_device(
                device_index=0,
                enable_external_usage=False,
            )

    def shutdown(self) -> None:
        shutdown_func = self._editor.contents.shutdown
        shutdown_func(self._editor)

    def update_camera(self, camera: Camera) -> None:
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
            cfg = self._get_or_default_config(config)
            self._ensure_device(cfg)
            show_func(
                self._editor,
                self._compute.device_interface().get_device(),
                byref(cfg),
            )
        except (OSError, ValueError, RuntimeError) as e:
            print(f"Error: Editor runtime error ({e})")

    def start(self, config=None) -> None:
        """Start the editor."""
        start_func = self._editor.contents.start

        try:
            cfg = self._get_or_default_config(config)
            self._ensure_device(cfg)
            start_func(
                self._editor,
                self._compute.device_interface().get_device(),
                byref(cfg),
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

    def get_token(self, name: str):
        """Get a token for a given name."""
        get_token_func = self._editor.contents.get_token
        return get_token_func(name.encode("utf-8"))

    def get_camera(self, scene):
        """Get camera for a given scene."""
        get_camera_func = self._editor.contents.get_camera
        return get_camera_func(self._editor, scene)

    def add_nanovdb_2(self, scene, name, array):
        """Add NanoVDB data to scene with token-based API."""
        add_nanovdb_2_func = self._editor.contents.add_nanovdb_2
        add_nanovdb_2_func(self._editor, scene, name, pointer(array))

    def add_gaussian_data_2(self, scene, name, desc):
        """Add Gaussian data to scene with token-based API."""
        add_gaussian_data_2_func = self._editor.contents.add_gaussian_data_2
        add_gaussian_data_2_func(self._editor, scene, name, pointer(desc))

    def update_camera_2(self, scene, camera):
        """Update camera for a scene with token-based API."""
        update_camera_2_func = self._editor.contents.update_camera_2
        update_camera_2_func(self._editor, scene, pointer(camera))

    def add_camera_view_2(self, scene, camera_view):
        """Add camera view to scene with token-based API."""
        add_camera_view_2_func = self._editor.contents.add_camera_view_2
        add_camera_view_2_func(self._editor, scene, pointer(camera_view))

    def remove(self, scene, name):
        """Remove an object from the scene."""
        remove_func = self._editor.contents.remove
        remove_func(self._editor, scene, name)

    def map_params(self, scene, name, data_type):
        """Map parameters for read/write access."""
        map_params_func = self._editor.contents.map_params
        return map_params_func(self._editor, scene, name, data_type)

    def unmap_params(self, scene, name):
        """Unmap parameters, flushing any writes."""
        unmap_params_func = self._editor.contents.unmap_params
        unmap_params_func(self._editor, scene, name)

    def get_resolved_port(self, should_wait: bool = False) -> int:
        """Get the resolved port for streaming."""
        get_resolved_port_func = self._editor.contents.get_resolved_port
        return get_resolved_port_func(self._editor, 1 if should_wait else 0)

    def __del__(self):
        self._editor = None
