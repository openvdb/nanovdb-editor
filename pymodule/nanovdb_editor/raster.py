# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from ctypes import *

from .utils import load_library
from .compute import pnanovdb_Compute, pnanovdb_ComputeArray
from .device import pnanovdb_Device, pnanovdb_ComputeQueue

COMPUTE_LIB = "pnanovdbcompute"


class pnanovdb_Raster(Structure):
    """Definition equivalent to pnanovdb_raster_t."""

    _fields_ = [
        ("interface_pnanovdb_reflect_data_type", c_void_p),
        ("compute", POINTER(pnanovdb_Compute)),
        (
            "create_context",
            CFUNCTYPE(
                c_void_p,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
            ),
        ),
        (
            "destroy_context",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,
            ),
        ),
        (
            "create_gaussian_data",
            CFUNCTYPE(
                c_void_p,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                POINTER(pnanovdb_ComputeArray),  # means
                POINTER(pnanovdb_ComputeArray),  # quaternions
                POINTER(pnanovdb_ComputeArray),  # scales
                POINTER(pnanovdb_ComputeArray),  # colors
                POINTER(pnanovdb_ComputeArray),  # sh_0
                POINTER(pnanovdb_ComputeArray),  # sh_n
                POINTER(pnanovdb_ComputeArray),  # opacities
                POINTER(POINTER(pnanovdb_ComputeArray)),  # shader_params_arrays
                c_void_p,  # raster_params
            ),
        ),
        (
            "upload_gaussian_data",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                c_void_p,  # data
            ),
        ),
        (
            "destroy_gaussian_data",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # data
            ),
        ),
        (
            "raster_gaussian_2d",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                c_void_p,  # data
                c_void_p,  # color_2d texture
                c_uint32,  # image_width
                c_uint32,  # image_height
                c_void_p,  # view matrix
                c_void_p,  # projection matrix
                c_void_p,  # shader_params
            ),
        ),
        (
            "raster_gaussian_3d",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                c_float,  # voxel_size
                c_void_p,  # data
                c_void_p,  # nanovdb_out buffer
                c_uint64,  # nanovdb_word_count
                c_void_p,  # userdata
            ),
        ),
        (
            "raster_to_nanovdb",
            CFUNCTYPE(
                POINTER(pnanovdb_ComputeArray),
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_float,  # voxel_size
                POINTER(pnanovdb_ComputeArray),  # means
                POINTER(pnanovdb_ComputeArray),  # quaternions
                POINTER(pnanovdb_ComputeArray),  # scales
                POINTER(pnanovdb_ComputeArray),  # colors
                POINTER(pnanovdb_ComputeArray),  # sh_0
                POINTER(pnanovdb_ComputeArray),  # sh_n
                POINTER(pnanovdb_ComputeArray),  # opacities
                POINTER(POINTER(pnanovdb_ComputeArray)),  # shader_params_arrays
                c_void_p,  # profiler_report
                c_void_p,  # userdata
            ),
        ),
        (
            "raster_file",
            CFUNCTYPE(
                c_int32,  # pnanovdb_bool_t
                c_void_p,  # pnanovdb_raster_t*
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_char_p,  # filename
                c_float,  # voxel_size
                POINTER(POINTER(pnanovdb_ComputeArray)),  # nanovdb_arr
                POINTER(c_void_p),  # gaussian_data
                POINTER(c_void_p),  # raster_context
                POINTER(POINTER(pnanovdb_ComputeArray)),  # shader_params_arrays
                c_void_p,  # raster_params
                c_void_p,  # profiler_report
                c_void_p,  # userdata
            ),
        ),
        (
            "raster_to_nanovdb_from_arrays",
            CFUNCTYPE(
                c_int32,  # pnanovdb_bool_t
                c_void_p,  # pnanovdb_raster_t*
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_float,  # voxel_size
                POINTER(POINTER(pnanovdb_ComputeArray)),  # arrays_gaussian
                c_uint32,  # array_count
                POINTER(POINTER(pnanovdb_ComputeArray)),  # out_nanovdb_arr
            ),
        ),
        (
            "create_gaussian_data_from_arrays",
            CFUNCTYPE(
                c_int32,  # pnanovdb_bool_t
                c_void_p,  # pnanovdb_raster_t*
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                POINTER(POINTER(pnanovdb_ComputeArray)),  # arrays_gaussian
                c_uint32,  # array_count
                POINTER(c_void_p),  # gaussian_data
                c_void_p,  # raster_params
                POINTER(c_void_p),  # raster_context
            ),
        ),
    ]


class Raster:
    """Python wrapper for pnanovdb_raster_t."""

    def __init__(
        self, compute: pnanovdb_Compute, device: pnanovdb_Device = None
    ):
        lib = load_library(COMPUTE_LIB)

        get_raster_func = lib.pnanovdb_get_raster
        get_raster_func.restype = POINTER(pnanovdb_Raster)
        get_raster_func.argtypes = []

        self._raster = get_raster_func()
        if not self._raster:
            raise RuntimeError("Failed to get raster")

        self._compute = compute
        self._device = device if device else compute.device_interface().get_device()
        self._compute_queue = compute.device_interface().get_compute_queue(
            self._device
        )
        self._raster.contents.compute = compute.get_compute()

    def raster_to_nanovdb(
        self,
        voxel_size: float,
        means: pnanovdb_ComputeArray,
        quaternions: pnanovdb_ComputeArray,
        scales: pnanovdb_ComputeArray,
        colors: pnanovdb_ComputeArray,
        sh_0: pnanovdb_ComputeArray,
        sh_n: pnanovdb_ComputeArray,
        opacities: pnanovdb_ComputeArray,
        shader_params_arrays=None,
        profiler_report=None,
        userdata=None,
    ) -> pnanovdb_ComputeArray:
        raster_to_nanovdb_func = self._raster.contents.raster_to_nanovdb

        try:
            nanovdb_array = raster_to_nanovdb_func(
                self._compute.get_compute(),
                self._compute_queue,
                voxel_size,
                pointer(means),
                pointer(quaternions),
                pointer(scales),
                pointer(colors),
                pointer(sh_0),
                pointer(sh_n),
                pointer(opacities),
                shader_params_arrays,
                profiler_report or c_void_p(),
                userdata or c_void_p(),
            )
        except Exception as e:
            print(f"Error rastering points: {e}")
            raise e

        if not nanovdb_array:
            raise RuntimeError("Failed to raster points")

        return nanovdb_array.contents

    def __del__(self):
        self._raster = None
        self._compute = None
