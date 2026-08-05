# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os

from contextlib import contextmanager
from typing import TYPE_CHECKING, Optional
from ctypes import (
    Structure,
    POINTER,
    CFUNCTYPE,
    c_void_p,
    c_char,
    c_char_p,
    c_int,
    c_int32,
    c_uint32,
    c_uint64,
    c_float,
    byref,
    create_string_buffer,
    pointer,
    sizeof,
)
import warnings

from .compute import Compute, pnanovdb_Compute, pnanovdb_ComputeArray
from .compiler import Compiler, pnanovdb_Compiler
from .device import pnanovdb_Device
from .exceptions import DeviceError, InvalidArgumentError, PipelineError
from .utils import load_library

if TYPE_CHECKING:
    from .scene import Scene

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
    """Definition equivalent to pnanovdb_editor_config_t.

    Prefer :func:`make_editor_config` (or kwargs on :meth:`Editor.show` /
    :meth:`Editor.start`) over setting ctypes fields by hand.
    """

    _fields_ = [
        ("ip_address", c_char_p),
        ("port", c_int32),
        ("headless", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("streaming", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("stream_to_file", c_int32),  # pnanovdb_bool_t is int32_t in C
        ("ui_profile_name", c_char_p),
    ]


def make_editor_config(
    config: Optional[EditorConfig] = None,
    *,
    ip: Optional[str] = None,
    port: Optional[int] = None,
    headless: Optional[bool] = None,
    streaming: Optional[bool] = None,
    stream_to_file: Optional[bool] = None,
    ui_profile: Optional[str] = None,
) -> EditorConfig:
    """Build an :class:`EditorConfig` from Python-friendly kwargs.

    Missing fields keep the defaults (``127.0.0.1:8080``, GUI, no streaming).
    Pass an existing ``config`` to start from, then override selected fields.
    """
    if config is None:
        cfg = EditorConfig()
        ip_default = b"127.0.0.1"
        cfg._keepalive = [ip_default]
        cfg.ip_address = ip_default
        cfg.port = 8080
        cfg.headless = 0
        cfg.streaming = 0
        cfg.stream_to_file = 0
        cfg.ui_profile_name = None
    else:
        cfg = EditorConfig()
        cfg._keepalive = []
        if config.ip_address:
            ip_bytes = bytes(config.ip_address)
            cfg._keepalive.append(ip_bytes)
            cfg.ip_address = ip_bytes
        else:
            cfg.ip_address = None
        cfg.port = config.port
        cfg.headless = config.headless
        cfg.streaming = config.streaming
        cfg.stream_to_file = config.stream_to_file
        if config.ui_profile_name:
            profile_bytes = bytes(config.ui_profile_name)
            cfg._keepalive.append(profile_bytes)
            cfg.ui_profile_name = profile_bytes
        else:
            cfg.ui_profile_name = None

    if ip is not None:
        if isinstance(ip, str):
            ip_bytes = ip.encode("utf-8")
        elif isinstance(ip, (bytes, bytearray)):
            ip_bytes = bytes(ip)
        else:
            raise InvalidArgumentError(f"ip must be str or bytes, got {type(ip).__name__}")
        if not ip_bytes:
            raise InvalidArgumentError("ip must be a non-empty address")
        cfg._keepalive = getattr(cfg, "_keepalive", [])
        cfg._keepalive.append(ip_bytes)
        cfg.ip_address = ip_bytes
    if port is not None:
        if isinstance(port, bool) or not isinstance(port, int):
            raise InvalidArgumentError(f"port must be an int, got {type(port).__name__}")
        if port < 0 or port > 65535:
            raise InvalidArgumentError(f"port must be in [0, 65535], got {port}")
        cfg.port = port
    if headless is not None:
        cfg.headless = 1 if headless else 0
    if streaming is not None:
        cfg.streaming = 1 if streaming else 0
    if stream_to_file is not None:
        cfg.stream_to_file = 1 if stream_to_file else 0
    if ui_profile is not None:
        if isinstance(ui_profile, str):
            profile_bytes = ui_profile.encode("utf-8")
        elif isinstance(ui_profile, (bytes, bytearray)):
            profile_bytes = bytes(ui_profile)
        else:
            raise InvalidArgumentError(f"ui_profile must be str or bytes, got {type(ui_profile).__name__}")
        cfg._keepalive = getattr(cfg, "_keepalive", [])
        cfg._keepalive.append(profile_bytes)
        cfg.ui_profile_name = profile_bytes
    return cfg


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


class pnanovdb_PipelineParams(Structure):
    """Definition equivalent to pnanovdb_pipeline_params_t.

    A thin descriptor returned by ``map_pipeline_params``: ``data`` points to the
    stage's parameter struct (e.g. ``GaussianVoxelizeParams``), ``size`` is its
    byte size, and ``type`` is an opaque reflected-type handle.
    """

    _fields_ = [
        ("data", c_void_p),
        ("size", c_uint64),
        ("type", c_void_p),
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
        ("reset", CFUNCTYPE(None, c_void_p)),
        ("wait_for_interrupt", CFUNCTYPE(None, c_void_p)),
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
        # Function pointers added after the Python wrapper's token API. Keep
        # opaque entries for APIs not wrapped here so later offsets stay exact.
        (
            "set_pipeline",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # pnanovdb_pipeline_stage_t
                c_uint32,  # pnanovdb_pipeline_type_t
            ),
        ),
        (
            "get_pipeline",
            CFUNCTYPE(
                c_uint32,  # pnanovdb_pipeline_type_t
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # pnanovdb_pipeline_stage_t
            ),
        ),
        (
            "mark_pipeline_dirty",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
            ),
        ),
        (
            "add_nanovdb_3",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                POINTER(pnanovdb_ComputeArray),  # array
                c_uint32,  # process_pipeline
                c_uint32,  # render_pipeline
            ),
        ),
        (
            "add_gaussian_data_3",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                POINTER(EditorGaussianDataDesc),  # desc
                c_uint32,  # process_pipeline
                c_uint32,  # render_pipeline
            ),
        ),
        (
            "set_visible",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                pnanovdb_bool_t,  # visible
            ),
        ),
        (
            "get_visible",
            CFUNCTYPE(
                pnanovdb_bool_t,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
            ),
        ),
        (
            "add_named_array",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # object_name
                POINTER(EditorToken),  # array_name
                POINTER(pnanovdb_ComputeArray),  # array
            ),
        ),
        (
            "get_named_array",
            CFUNCTYPE(
                POINTER(pnanovdb_ComputeArray),
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # object_name
                POINTER(EditorToken),  # array_name
            ),
        ),
        (
            "map_pipeline_params",
            CFUNCTYPE(
                POINTER(pnanovdb_PipelineParams),
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # pnanovdb_pipeline_stage_t
            ),
        ),
        (
            "unmap_pipeline_params",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # pnanovdb_pipeline_stage_t
            ),
        ),
        (
            "set_custom_scene_params",
            CFUNCTYPE(
                pnanovdb_bool_t,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # json (str carries the JSON payload)
                # Mutable out-buffer: must not use c_char_p (ctypes treats that
                # as an immutable C string and can drop writes / corrupt ABI).
                POINTER(c_char),  # error_buf
                c_uint64,  # error_buf_size
            ),
        ),
        (
            "get_custom_scene_params_data_type",
            CFUNCTYPE(
                c_void_p,  # const pnanovdb_reflect_data_type_t* (opaque handle)
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
            ),
        ),
        (
            "get_process_step_count",
            CFUNCTYPE(
                c_uint32,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
            ),
        ),
        (
            "get_process_step",
            CFUNCTYPE(
                c_uint32,  # pnanovdb_pipeline_type_t
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # step_index
            ),
        ),
        (
            "set_process_step",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # step_index
                c_uint32,  # pnanovdb_pipeline_type_t
            ),
        ),
        (
            "map_process_step_params",
            CFUNCTYPE(
                POINTER(pnanovdb_PipelineParams),
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # step_index
            ),
        ),
        (
            "unmap_process_step_params",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # step_index
            ),
        ),
        ("load_scene", CFUNCTYPE(c_int32, c_void_p, c_char_p, c_int32)),
        ("save_scene", CFUNCTYPE(c_int32, c_void_p, c_char_p)),
        ("get_pipeline_type", CFUNCTYPE(c_int32, c_void_p, POINTER(EditorToken), POINTER(c_uint32))),
        (
            "get_camera_2",
            CFUNCTYPE(
                pnanovdb_bool_t,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(Camera),  # out_camera (caller-owned)
            ),
        ),
        (
            "add_gaussian_data_4",
            CFUNCTYPE(
                None,
                c_void_p,  # pnanovdb_editor_t*
                POINTER(EditorToken),  # scene
                POINTER(EditorToken),  # name
                c_uint32,  # process_pipeline
                c_uint32,  # render_pipeline
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

    def __init__(self, compute: Compute, compiler: Compiler, *, device_index: int = 0):
        self._lib = load_library(EDITOR_LIB)

        get_editor = self._lib.pnanovdb_get_editor
        get_editor.restype = POINTER(pnanovdb_Editor)
        get_editor.argtypes = []

        self._editor = get_editor()
        if not self._editor:
            raise PipelineError("Failed to get editor interface")

        self._compute = compute
        self._compiler = compiler
        # Device to create if none exists yet when show/start or a pipeline
        # helper needs one; keeps lazy creation on the caller's chosen GPU.
        self._device_index = int(device_index)

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

        # Cache for last added arrays (avoid relying on impl layout)
        self._last_nanovdb_array = None
        self._last_data_array = None

        # Lazily created raster/voxelbvh interfaces, shared across scene helpers.
        self._raster = None
        self._voxelbvh = None

        # Custom scene params hot-reload bookkeeping, keyed by scene token id:
        #   _custom_params_files: {scene_id: (abs_path, last_mtime)}
        self._custom_params_files = {}

    def _get_or_default_config(
        self,
        config: Optional[EditorConfig] = None,
        **kwargs,
    ) -> EditorConfig:
        return make_editor_config(config, **kwargs)

    def _ensure_device(self, _config: Optional[EditorConfig] = None) -> None:
        di = self._compute.device_interface()
        try:
            di.get_device()
            return
        except DeviceError:
            pass
        di.create_device_manager(False)
        di.create_device(
            device_index=self._device_index,
            enable_external_usage=False,
        )

    def shutdown(self) -> bool:
        """Stop any worker and tear down the native editor implementation.

        Idempotent: safe to call more than once, including after :meth:`close`.
        Prefer :meth:`close` from Python — it also drops the ctypes handle once
        teardown has actually completed.

        Returns:
            True when the native implementation is gone. False when teardown was
            deferred because this was called from the active render thread (call
            again from another thread after the render loop exits), or when the
            native call failed and the implementation may still be alive.
        """
        editor = getattr(self, "_editor", None)
        if not editor:
            return True
        try:
            shutdown_func = editor.contents.shutdown
            if shutdown_func:
                shutdown_func(editor)
            # Same rule as pnanovdb_editor_free: a surviving impl means the
            # native side deferred teardown while show() is still running.
            return not editor.contents.impl
        except Exception:
            # Never raise from teardown, but don't report success either: the
            # implementation may still be alive and holding the compiler.
            return False

    def reset(self) -> None:
        editor = getattr(self, "_editor", None)
        if not editor:
            return
        reset_func = getattr(editor.contents, "reset", None)
        if reset_func:
            reset_func(editor)

    def wait_for_interrupt(self) -> None:
        editor = getattr(self, "_editor", None)
        if not editor:
            return
        wait_func = getattr(editor.contents, "wait_for_interrupt", None)
        if wait_func:
            wait_func(editor)

    def update_camera(self, camera: Camera) -> None:
        udpate_camera_func = self._editor.contents.update_camera
        udpate_camera_func(self._editor, pointer(camera))

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

    def show(self, config: Optional[EditorConfig] = None, **kwargs) -> None:
        """Run the editor UI (blocking until the window closes).

        Pass an :class:`EditorConfig`, or Python-friendly kwargs accepted by
        :func:`make_editor_config` (``ip``, ``port``, ``headless``,
        ``streaming``, ``stream_to_file``, ``ui_profile``).

        Raises:
            DeviceError / PipelineError: On device or runtime failure.
        """
        show_func = self._editor.contents.show
        try:
            cfg = self._get_or_default_config(config, **kwargs)
            self._ensure_device(cfg)
            show_func(
                self._editor,
                self._compute.device_interface().get_device(),
                byref(cfg),
            )
        except DeviceError:
            raise
        except (OSError, ValueError, RuntimeError) as e:
            raise PipelineError(f"Editor runtime error: {e}") from e

    def start(self, config: Optional[EditorConfig] = None, **kwargs) -> None:
        """Start the editor worker (non-blocking; typically used headless).

        Prefer :meth:`show` for an interactive GUI. Use :meth:`start` with
        ``headless=True`` / ``streaming=True``, then :meth:`wait_for_interrupt`
        (or :meth:`Session.run`).

        Raises:
            DeviceError / PipelineError: On device or start failure.
        """
        start_func = self._editor.contents.start
        try:
            cfg = self._get_or_default_config(config, **kwargs)
            self._ensure_device(cfg)
            start_func(
                self._editor,
                self._compute.device_interface().get_device(),
                byref(cfg),
            )
        except DeviceError:
            raise
        except (OSError, ValueError, RuntimeError) as e:
            raise PipelineError(f"Editor start error: {e}") from e

    def stop(self) -> None:
        """Stop the editor worker/render loop.

        Idempotent: a no-op when the editor is already stopped or closed.
        """
        editor = getattr(self, "_editor", None)
        if not editor:
            return
        try:
            stop_func = editor.contents.stop
            if stop_func:
                stop_func(editor)
        except Exception:
            pass

    def close(self) -> bool:
        """Stop the worker (if any), shut down the native editor, and drop handles.

        Safe to call more than once. After a completed close, further API calls
        that need the native editor will fail or no-op. Prefer this (or a
        :class:`~nanovdb_editor.Session` ``with`` block) over relying on GC.

        Returns:
            True when teardown completed and the handle was dropped. False when
            the native side deferred teardown (``close`` was called from the
            active render thread); the handle is kept so a later call from
            another thread can finish the job.
        """
        editor = getattr(self, "_editor", None)
        if editor is None:
            return True
        # Drop lazily-created pipeline wrappers before native teardown so they
        # cannot call into a dying editor from their finalizers.
        self._raster = None
        self._voxelbvh = None
        if not self.shutdown():
            return False
        self._editor = None
        return True

    def load_scene(self, filepath, overwrite: bool = False) -> None:
        """Load a scene through the active editor worker.

        When overwrite is True, any existing scene whose name collides with the
        file is replaced; otherwise colliding names are merged/skipped.

        Raises:
            PipelineError: If the native load fails.
        """
        ok = bool(self._editor.contents.load_scene(self._editor, os.fsencode(filepath), 1 if overwrite else 0))
        if not ok:
            raise PipelineError(f"Failed to load scene: {filepath}")

    def save_scene(self, filepath) -> None:
        """Save all scenes through the active editor worker.

        Raises:
            PipelineError: If the native save fails.
        """
        ok = bool(self._editor.contents.save_scene(self._editor, os.fsencode(filepath)))
        if not ok:
            raise PipelineError(f"Failed to save scene: {filepath}")

    def get_pipeline_type(self, name_token) -> int:
        """Resolve a pipeline enum value from its token string."""
        pipeline_type = c_uint32()
        ok = self._editor.contents.get_pipeline_type(self._editor, name_token, byref(pipeline_type))
        if not ok:
            raise InvalidArgumentError(f"Unknown pipeline type: {name_token.contents.str.decode()!r}")
        return int(pipeline_type.value)

    def get_nanovdb(self) -> pnanovdb_ComputeArray:
        warnings.warn(
            "Editor.get_nanovdb() is legacy; prefer Scene + Grid helpers",
            DeprecationWarning,
            stacklevel=2,
        )
        if self._last_nanovdb_array is None:
            raise PipelineError("No NanoVDB array available")
        return self._last_nanovdb_array

    def get_array(self) -> pnanovdb_ComputeArray:
        warnings.warn(
            "Editor.get_array() is legacy; prefer Scene + Grid helpers",
            DeprecationWarning,
            stacklevel=2,
        )
        if self._last_data_array is None:
            raise PipelineError("No data array available")
        return self._last_data_array

    def add_callable(self, name: str, func) -> None:
        """Compatibility stub for older API; no-op in current interface."""
        warnings.warn(
            "Editor.add_callable() is a no-op legacy stub and will be removed",
            DeprecationWarning,
            stacklevel=2,
        )
        _ = (name, func)

    def add_nanovdb(self, array: pnanovdb_ComputeArray) -> None:
        warnings.warn(
            "Editor.add_nanovdb() is obsolete; use scene.add_grid(...) or add_nanovdb_2",
            DeprecationWarning,
            stacklevel=2,
        )
        add_nanovdb_func = self._editor.contents.add_nanovdb
        add_nanovdb_func(self._editor, pointer(array))
        self._last_nanovdb_array = array

    def add_array(self, array: pnanovdb_ComputeArray) -> None:
        warnings.warn(
            "Editor.add_array() is obsolete; prefer Scene helpers",
            DeprecationWarning,
            stacklevel=2,
        )
        add_array_func = self._editor.contents.add_array
        add_array_func(self._editor, pointer(array))
        self._last_data_array = array

    def get_token(self, name: str):
        """Get a token for a given name."""
        get_token_func = self._editor.contents.get_token
        return get_token_func(name.encode("utf-8"))

    def scene(self, name: str) -> "Scene":
        """Return a high-level handle for the named scene.

        ``Scene`` is imported lazily so ``editor`` and ``scene`` never form a
        module-load cycle (``scene`` only type-checks ``Editor``).

        Example:
            nvdb = editor.scene("main").nanovdb_from_gaussians(
                means=means, quats=quats, scales=scales,
                sh_0=sh_0, sh_n=sh_n, opacities=opacities,
                process="raster3d", voxel_size=1 / 128,
            )
        """
        from .scene import Scene

        return Scene(self, name)

    def _ensure_device_created(self):
        """Ensure a compute device exists before building pipeline interfaces."""
        self._ensure_device()

    def _get_raster(self):
        """Return a shared Raster interface, creating it on first use."""
        if self._raster is None:
            from .raster import Raster

            self._ensure_device_created()
            self._raster = Raster(self._compute)
        return self._raster

    def _get_voxelbvh(self):
        """Return a shared VoxelBVH interface, creating it on first use."""
        if self._voxelbvh is None:
            from .voxelbvh import VoxelBVH

            self._ensure_device_created()
            self._voxelbvh = VoxelBVH(self._compute)
        return self._voxelbvh

    def get_camera(self, scene):
        """Get camera for a given scene."""
        get_camera_func = self._editor.contents.get_camera
        return get_camera_func(self._editor, scene)

    def get_camera_2(self, scene):
        """Get a copy of the camera for a given scene.

        Returns a fresh Camera value (safe to keep and to call concurrently), or
        None if the scene has not been seen yet.
        """
        camera = Camera()
        get_camera_2_func = self._editor.contents.get_camera_2
        found = get_camera_2_func(self._editor, scene, byref(camera))
        return camera if found else None

    def add_nanovdb_2(self, scene, name, array):
        """Add NanoVDB data to scene with token-based API."""
        add_nanovdb_2_func = self._editor.contents.add_nanovdb_2
        add_nanovdb_2_func(self._editor, scene, name, pointer(array))

    def add_gaussian_data_2(self, scene, name, desc):
        """Add Gaussian data to scene with token-based API."""
        add_gaussian_data_2_func = self._editor.contents.add_gaussian_data_2
        add_gaussian_data_2_func(self._editor, scene, name, pointer(desc))

    def add_named_array(self, scene, object_name, array_name, array):
        """Attach a named array to a scene object (creating the object if needed)."""
        add_named_array_func = self._editor.contents.add_named_array
        add_named_array_func(self._editor, scene, object_name, array_name, pointer(array))

    def add_gaussian_data_4(self, scene, name, process_pipeline, render_pipeline):
        """Create Gaussian data from named arrays previously attached to the object.

        Attach the conventional arrays first via add_named_array using the names
        "means", "opacities", "quaternions", "scales", "sh_0", and optionally "sh_n".
        """
        add_gaussian_data_4_func = self._editor.contents.add_gaussian_data_4
        add_gaussian_data_4_func(self._editor, scene, name, int(process_pipeline), int(render_pipeline))

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
        """Map parameters for read/write access.

        Prefer :meth:`params`, which pairs the map with an automatic unmap.
        """
        map_params_func = self._editor.contents.map_params
        return map_params_func(self._editor, scene, name, data_type)

    def unmap_params(self, scene, name):
        """Unmap parameters, flushing any writes."""
        unmap_params_func = self._editor.contents.unmap_params
        unmap_params_func(self._editor, scene, name)

    @contextmanager
    def params(self, scene, name, data_type):
        """Context manager over a scene object's mapped parameters.

        Yields whatever :meth:`map_params` returns for ``data_type`` and always
        calls :meth:`unmap_params` on exit, flushing any writes.

        Example::

            with editor.params(scene_token, image_token, shader_type) as p:
                p.shader_name = editor.get_token("editor/image2d.slang")
        """
        mapped = self.map_params(scene, name, data_type)
        try:
            yield mapped
        finally:
            self.unmap_params(scene, name)

    @staticmethod
    def _resolve_pipeline_type(pipeline) -> int:
        """Coerce a pipeline reference to its integer enum value.

        Accepts an ``int`` enum value, a ``process`` alias / enum name string, or
        a :class:`~nanovdb_editor.pipelines.PipelineInfo`.
        """
        if isinstance(pipeline, int):
            return int(pipeline)
        value = getattr(pipeline, "value", None)
        if value is not None:
            return int(value)
        from .exceptions import InvalidArgumentError
        from .pipelines import get_pipeline_info

        info = get_pipeline_info(pipeline)
        if info is None:
            raise InvalidArgumentError(f"Unknown pipeline: {pipeline!r}")
        return info.value

    def set_pipeline(self, scene, name, stage: int, pipeline) -> None:
        """Assign a pipeline to a scene object's ``load``/``process``/``render`` stage.

        Args:
            scene: Scene token (from ``get_token``).
            name: Object name token (from ``get_token``).
            stage: One of ``PIPELINE_STAGE_LOAD``/``PROCESS``/``RENDER``.
            pipeline: Pipeline enum value, ``process`` alias / enum name, or a
                ``PipelineInfo``.
        """
        set_pipeline_func = self._editor.contents.set_pipeline
        set_pipeline_func(
            self._editor,
            scene,
            name,
            c_uint32(int(stage)),
            c_uint32(self._resolve_pipeline_type(pipeline)),
        )

    def get_pipeline(self, scene, name, stage: int) -> int:
        """Return the pipeline enum value bound to a scene object's stage."""
        get_pipeline_func = self._editor.contents.get_pipeline
        return int(get_pipeline_func(self._editor, scene, name, c_uint32(int(stage))))

    def mark_pipeline_dirty(self, scene, name) -> None:
        """Force a scene object's pipelines to re-run on the next update."""
        mark_dirty_func = self._editor.contents.mark_pipeline_dirty
        mark_dirty_func(self._editor, scene, name)

    def map_pipeline_params(self, scene, name, stage: int):
        """Map a stage's pipeline parameters for read/write access.

        Returns a ``POINTER(pnanovdb_PipelineParams)`` (may be null). You MUST
        call :meth:`unmap_pipeline_params` for the same stage afterwards, even
        when the returned pointer is null; :meth:`pipeline_params` wraps both in
        a context manager and should be preferred.
        """
        map_func = self._editor.contents.map_pipeline_params
        return map_func(self._editor, scene, name, c_uint32(int(stage)))

    def unmap_pipeline_params(self, scene, name, stage: int) -> None:
        """Release a stage's pipeline parameters and mark the stage dirty."""
        unmap_func = self._editor.contents.unmap_pipeline_params
        unmap_func(self._editor, scene, name, c_uint32(int(stage)))

    @contextmanager
    def pipeline_params(self, scene, name, stage: int):
        """Context manager yielding a stage's ``pnanovdb_pipeline_params_t``.

        Yields the mapped ``pnanovdb_PipelineParams`` (or ``None`` when the stage
        exposes no parameters) and always calls ``unmap_pipeline_params`` on exit,
        which flushes writes and marks the stage dirty.

        Example::

            with editor.pipeline_params(scene, name, PIPELINE_STAGE_PROCESS) as p:
                if p is not None and p.size >= 4:
                    # p.data points to the stage's parameter struct
                    ...
        """
        params_ptr = self.map_pipeline_params(scene, name, stage)
        try:
            yield params_ptr.contents if params_ptr else None
        finally:
            self.unmap_pipeline_params(scene, name, stage)

    # ------------------------------------------------------------------
    # Multi-step process chains
    # ------------------------------------------------------------------

    def get_process_step_count(self, scene, name) -> int:
        """Return how many process steps an object currently has."""
        func = self._editor.contents.get_process_step_count
        return int(func(self._editor, scene, name))

    def get_process_step(self, scene, name, step_index: int) -> int:
        """Return the pipeline enum value at ``step_index`` (``noop`` if OOB)."""
        func = self._editor.contents.get_process_step
        return int(func(self._editor, scene, name, c_uint32(int(step_index))))

    def set_process_step(self, scene, name, step_index: int, pipeline) -> None:
        """Set (or append) a process-step pipeline on a scene object.

        When ``step_index`` equals the current step count, a new step is
        appended. The index is validated before the native call; acceptance of
        the pipeline type is left to the native side and confirmed by a short
        post-check (so a stale Python registry cannot reject a still-valid C
        pipeline or paper over a native rejection).

        Raises:
            PipelineError: If the native API rejects the request (chain
                template, non-process pipeline, out-of-range index, etc.).
        """
        step_index = int(step_index)
        if step_index < 0:
            raise PipelineError(f"process step index {step_index} out of range")

        resolved = self._resolve_pipeline_type(pipeline)
        count_before = self.get_process_step_count(scene, name)
        if step_index > count_before:
            raise PipelineError(f"process step index {step_index} out of range for chain of length {count_before}")

        func = self._editor.contents.set_process_step
        func(
            self._editor,
            scene,
            name,
            c_uint32(step_index),
            c_uint32(resolved),
        )

        count_after = self.get_process_step_count(scene, name)
        if step_index == count_before:
            if count_after != count_before + 1:
                raise PipelineError(f"Failed to append process step {pipeline!r}")
        elif count_after != count_before:
            raise PipelineError(f"Unexpected process-chain length after updating step {step_index}")

        if self.get_process_step(scene, name, step_index) != resolved:
            raise PipelineError(f"Failed to set process step {step_index} to {pipeline!r}")

    def map_process_step_params(self, scene, name, step_index: int):
        """Map a process step's parameters for read/write access.

        Prefer :meth:`process_step_params`, which pairs the map with an automatic
        unmap.
        """
        func = self._editor.contents.map_process_step_params
        return func(self._editor, scene, name, c_uint32(int(step_index)))

    def unmap_process_step_params(self, scene, name, step_index: int) -> None:
        """Release a process step's parameters and mark the step dirty."""
        func = self._editor.contents.unmap_process_step_params
        func(self._editor, scene, name, c_uint32(int(step_index)))

    @contextmanager
    def process_step_params(self, scene, name, step_index: int):
        """Context manager yielding a process step's ``pnanovdb_pipeline_params_t``.

        Yields the mapped ``pnanovdb_PipelineParams`` (or ``None`` when the step
        exposes no parameters) and always calls ``unmap_process_step_params`` on
        exit, which flushes writes and marks the step dirty.

        Example::

            with editor.process_step_params(scene, name, 0) as p:
                if p is not None and p.size >= 4:
                    ...  # p.data points at the step's parameter struct
        """
        params_ptr = self.map_process_step_params(scene, name, step_index)
        try:
            yield params_ptr.contents if params_ptr else None
        finally:
            self.unmap_process_step_params(scene, name, step_index)

    def set_custom_scene_params(self, scene, json_string) -> None:
        """Attach scene-level custom UI params described by a JSON payload.

        The JSON must contain an object-valued ``"SceneParams"`` entry; see
        ``CustomSceneParams`` on the C++ side for the supported field schema
        (``type``, ``value``, ``min``, ``max``, ``elementCount``, ...).

        Args:
            scene: Scene token (from ``get_token``).
            json_string: The JSON payload as ``str`` or ``bytes``.

        Raises:
            PipelineError: If the payload is rejected (message contains the
                server-provided reason).
        """
        if isinstance(json_string, bytes):
            json_string = json_string.decode("utf-8")

        # The C API carries the JSON payload in the token's string field.
        # Failures are reported via the bool return + error_buf (native code
        # catches C++ exceptions at the ABI boundary).
        json_token = self.get_token(json_string)
        error_buf = create_string_buffer(1024)
        ok = self._editor.contents.set_custom_scene_params(
            self._editor,
            scene,
            json_token,
            error_buf,
            c_uint64(sizeof(error_buf)),
        )
        if not ok:
            message = error_buf.value.decode("utf-8", "replace") or "set_custom_scene_params failed"
            raise PipelineError(message)

    def set_custom_scene_params_from_file(self, scene, filepath) -> None:
        """Attach scene-level custom params from a JSON file on disk.

        Also records the file for later hot-reload via
        ``reload_custom_scene_params_if_changed`` / ``start_custom_scene_params_watch``.
        """
        filepath = os.path.abspath(os.fspath(filepath))
        with open(filepath, "r", encoding="utf-8") as f:
            json_string = f.read()
        self.set_custom_scene_params(scene, json_string)
        self._custom_params_files[scene.contents.id] = (filepath, os.path.getmtime(filepath))

    def get_custom_scene_params_data_type(self, scene):
        """Return an opaque handle to a scene's custom-params reflected data type.

        The returned value is the ``const pnanovdb_reflect_data_type_t*`` used by
        the C API (a Python int address, or ``None`` when no custom params are
        attached). It can be passed straight back into ``map_params`` as the
        ``data_type`` argument.
        """
        return self._editor.contents.get_custom_scene_params_data_type(self._editor, scene)

    def reload_custom_scene_params_if_changed(self, scene, filepath=None) -> bool:
        """Re-apply custom params from disk if the JSON file changed.

        Mirrors the editor's shader-param file reload behavior: it compares the
        file modification time against the last applied value and only re-loads
        when it advances.

        Args:
            scene: Scene token.
            filepath: JSON path; defaults to the file previously registered via
                ``set_custom_scene_params_from_file``.

        Returns:
            True if the params were reloaded, False otherwise.
        """
        key = scene.contents.id
        entry = self._custom_params_files.get(key)
        if filepath is None:
            if entry is None:
                return False
            filepath = entry[0]
        filepath = os.path.abspath(os.fspath(filepath))

        try:
            mtime = os.path.getmtime(filepath)
        except OSError:
            return False

        if entry is not None and entry[0] == filepath and mtime <= entry[1]:
            return False

        self.set_custom_scene_params_from_file(scene, filepath)
        return True

    def get_resolved_port(self, should_wait=None, *, wait: bool = False) -> int:
        """Get the resolved port for streaming.

        Args:
            wait: When True, block until a port is resolved.
            should_wait: Deprecated alias kept for callers that pass the flag
                positionally.
        """
        if should_wait is not None:
            warnings.warn(
                "get_resolved_port(should_wait) is deprecated; use get_resolved_port(wait=...)",
                DeprecationWarning,
                stacklevel=2,
            )
            wait = bool(should_wait)
        get_resolved_port_func = self._editor.contents.get_resolved_port
        return get_resolved_port_func(self._editor, 1 if wait else 0)

    def add_image2d(self, scene, name, image_data, width: int, height: int):
        """Add a 2D image to the editor.

        Converts RGBA8 image data to NanoVDB format and adds it to the specified scene.

        Args:
            scene: Scene token (from get_token)
            name: Name token for the image (from get_token)
            image_data: pnanovdb_ComputeArray with RGBA8 image data (uint32 per pixel, packed as RGBA)
            width: Image width in pixels
            height: Image height in pixels

        Example:
            # Create image data
            import numpy as np
            width, height = 1440, 720
            image_rgba = np.zeros((height, width), dtype=np.uint32)
            for j in range(height):
                for i in range(width):
                    r = (255 * i) // (width - 1)
                    g = (255 * j) // (height - 1)
                    b = 0
                    a = 255
                    image_rgba[j, i] = r | (g << 8) | (b << 16) | (a << 24)

            # Create compute array and add to editor
            image_array = compute.create_array(image_rgba)
            scene_token = editor.get_token("main")
            image_token = editor.get_token("my_image")
            editor.add_image2d(scene_token, image_token, image_array, width, height)
            compute.destroy_array(image_array)

        Note:
            To set a custom shader for the image, use the ``params`` context
            manager with the shader name after adding:
                shader_type = ...  # get reflection type for pnanovdb_editor_shader_name_t
                with editor.params(scene_token, image_token, shader_type) as mapped:
                    mapped.shader_name = editor.get_token("editor/image2d.slang")
        """
        # Convert image data to NanoVDB format
        image_nanovdb = self._compute.nanovdb_from_image_rgba8(image_data, width, height)

        # Add to the editor
        self.add_nanovdb_2(scene, name, image_nanovdb)

        # Clean up the converted array (the editor has made a copy)
        self._compute.destroy_array(image_nanovdb)

    def __del__(self):
        # Drop the ctypes handle only — never call into the native library from
        # a finalizer (threads / interpreter state may already be torn down).
        # Explicit :meth:`close` / :class:`~nanovdb_editor.Session` is required
        # for a clean native shutdown.
        try:
            self._editor = None
        except Exception:
            pass
