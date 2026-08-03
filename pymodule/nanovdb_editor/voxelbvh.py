# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Python wrapper for the ``pnanovdb_voxelbvh_t`` interface.

The VoxelBVH interface (``voxelbvh_build`` process pipeline) builds NanoVDB
grids from Gaussian splats, triangle meshes or line sets. It is loaded from the
same ``pnanovdbcompute`` shared library as the raster interface.
"""

from ctypes import (
    Structure,
    POINTER,
    CFUNCTYPE,
    c_void_p,
    c_char_p,
    c_uint32,
    c_float,
    byref,
    pointer,
)

from .utils import load_library
from .compute import pnanovdb_Compute, pnanovdb_ComputeArray
from .device import pnanovdb_Device, pnanovdb_ComputeQueue
from .exceptions import InvalidArgumentError, PipelineError

COMPUTE_LIB = "pnanovdbcompute"

# Defaults mirrored from editor/PipelineTypes.h (pnanovdb_editor namespace).
DEFAULT_BVH_RESOLUTION = 512
MAX_BVH_RESOLUTION = 4096

# RGBA8 conversion defaults, mirrored from editor/PipelineTypes.h.
DEFAULT_RGBA8_UPSAMPLE = 2
MAX_RGBA8_UPSAMPLE = 4

# NanoVDB grid type for a packed-RGBA8 image grid (nanovdb::GridType::RGBA8).
PNANOVDB_GRID_TYPE_RGBA8 = 12

# Default index-space ray direction used to bake colors (matches the editor).
DEFAULT_RGBA8_RAY_DIRECTION = (0.0, 0.0, -1.0)

# Default multi-direction bake set, mirrored from editor/Pipeline.cpp
# (``k_voxelbvh_rgba8_multi_dirs``). The editor treats these as world-space
# directions and converts them to index space; from Python they are passed
# straight through as index-space directions (identical for identity maps).
DEFAULT_RGBA8_DIRECTIONS = (
    (-1.0, -1.0, 0.0),
    (1.0, -1.0, 0.0),
    (-1.0, 1.0, 0.0),
    (1.0, 1.0, 0.0),
    (-1.0, 0.0, 0.0),
    (0.0, -1.0, 0.0),
    (1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0),
)

_ComputeArrayPtr = POINTER(pnanovdb_ComputeArray)


class pnanovdb_Vec3(Structure):
    """Definition equivalent to pnanovdb_vec3_t."""

    _fields_ = [("x", c_float), ("y", c_float), ("z", c_float)]


class pnanovdb_VoxelBVH(Structure):
    """Definition equivalent to pnanovdb_voxelbvh_t.

    Only the entry points used from Python are given full function signatures;
    the remaining function pointers are kept as opaque ``c_void_p`` placeholders
    so field offsets stay in sync with the C struct layout.
    """

    _fields_ = [
        ("interface_pnanovdb_reflect_data_type", c_void_p),
        ("compute", POINTER(pnanovdb_Compute)),
        (
            "create_context",
            CFUNCTYPE(c_void_p, POINTER(pnanovdb_Compute), POINTER(pnanovdb_ComputeQueue)),
        ),
        (
            "destroy_context",
            CFUNCTYPE(None, POINTER(pnanovdb_Compute), POINTER(pnanovdb_ComputeQueue), c_void_p),
        ),
        ("nanovdb_generate_node_mask", c_void_p),
        ("nanovdb_generate_node_mask_array", c_void_p),
        ("nanovdb_init", c_void_p),
        ("nanovdb_add_nodes", c_void_p),
        ("nanovdb_add_nodes_from_ijkl_buffer", c_void_p),
        ("nanovdb_add_nodes_from_ijkl_array", c_void_p),
        ("ijkl_from_gaussians", c_void_p),
        ("ijkl_from_gaussians_file", c_void_p),
        ("nanovdb_append_metadata", c_void_p),
        ("ijkl_from_lines", c_void_p),
        ("ijkl_from_lines_array", c_void_p),
        ("ijkl_from_triangles", c_void_p),
        ("ijkl_from_triangles_array", c_void_p),
        (
            "nanovdb_from_gaussians_file",
            CFUNCTYPE(
                _ComputeArrayPtr,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                c_char_p,  # filename
                c_uint32,  # resolution
            ),
        ),
        (
            "nanovdb_from_gaussians_array",
            CFUNCTYPE(
                _ComputeArrayPtr,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                POINTER(_ComputeArrayPtr),  # gaussian_arrays
                c_uint32,  # gaussian_array_count (must be 6)
                c_uint32,  # resolution
            ),
        ),
        (
            "nanovdb_from_triangles_array",
            CFUNCTYPE(
                _ComputeArrayPtr,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                _ComputeArrayPtr,  # indices
                _ComputeArrayPtr,  # positions
                _ComputeArrayPtr,  # colors
                c_float,  # inflation_radius
                c_uint32,  # resolution
            ),
        ),
        (
            "nanovdb_from_lines_array",
            CFUNCTYPE(
                _ComputeArrayPtr,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                _ComputeArrayPtr,  # indices
                _ComputeArrayPtr,  # positions
                _ComputeArrayPtr,  # colors
                c_float,  # inflation_radius
                c_uint32,  # resolution
            ),
        ),
        ("nanovdb_duplicate_topology", c_void_p),
        (
            "nanovdb_duplicate_topology_array",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                POINTER(_ComputeArrayPtr),  # dst_nanovdb_out (allocated by callee)
                _ComputeArrayPtr,  # src_nanovdb_in
                c_uint32,  # dst_grid_type
                c_uint32,  # upsample_factor
            ),
        ),
        ("nanovdb_rgba8_from_voxelbvh", c_void_p),
        (
            "nanovdb_rgba8_from_voxelbvh_array",
            CFUNCTYPE(
                None,
                POINTER(pnanovdb_Compute),
                POINTER(pnanovdb_ComputeQueue),
                c_void_p,  # context
                _ComputeArrayPtr,  # dst_nanovdb_inout
                _ComputeArrayPtr,  # src_nanovdb_in
                pnanovdb_Vec3,  # index_space_ray_direction
            ),
        ),
        ("context_set_cancel", c_void_p),
        ("context_set_progress", c_void_p),
    ]


class VoxelBVH:
    """Python wrapper for the ``pnanovdb_voxelbvh_t`` interface.

    Builds NanoVDB grids from Gaussian splats, triangle meshes or line sets via
    the ``voxelbvh_build`` process pipeline. A single reusable build context is
    created lazily on first use and released on destruction.
    """

    def __init__(self, compute, device: pnanovdb_Device = None):
        lib = load_library(COMPUTE_LIB)

        get_voxelbvh_func = lib.pnanovdb_get_voxelbvh
        get_voxelbvh_func.restype = POINTER(pnanovdb_VoxelBVH)
        get_voxelbvh_func.argtypes = []

        self._voxelbvh = get_voxelbvh_func()
        if not self._voxelbvh:
            raise PipelineError("Failed to get voxelbvh")

        self._compute = compute
        self._device = device if device else compute.device_interface().get_device()
        self._compute_queue = compute.device_interface().get_compute_queue(self._device)
        self._voxelbvh.contents.compute = compute.get_compute()
        self._context = None

    def _ensure_context(self):
        if self._context is None:
            create_context = self._voxelbvh.contents.create_context
            self._context = create_context(self._compute.get_compute(), self._compute_queue)
            if not self._context:
                raise PipelineError("Failed to create voxelbvh context")
        return self._context

    @staticmethod
    def _clamp_resolution(resolution):
        resolution = int(resolution)
        if resolution < 1 or resolution > MAX_BVH_RESOLUTION:
            raise InvalidArgumentError(f"resolution must be in [1, {MAX_BVH_RESOLUTION}], got {resolution}")
        return resolution

    def nanovdb_from_gaussians_array(
        self,
        arrays,
        resolution: int = DEFAULT_BVH_RESOLUTION,
    ) -> pnanovdb_ComputeArray:
        """Build a NanoVDB grid from 6 raw Gaussian arrays.

        Args:
            arrays: Exactly six ``pnanovdb_ComputeArray`` in the canonical order
                ``[means, opacities, quaternions, scales, sh_0, sh_n]``.
            resolution: VoxelBVH grid resolution (1..4096).
        """
        if len(arrays) != 6:
            raise InvalidArgumentError(
                "VoxelBVH gaussians requires 6 arrays: " "[means, opacities, quaternions, scales, sh_0, sh_n]"
            )
        resolution = self._clamp_resolution(resolution)
        ctx = self._ensure_context()

        arrays_c = (_ComputeArrayPtr * 6)(*[pointer(a) for a in arrays])
        func = self._voxelbvh.contents.nanovdb_from_gaussians_array
        out = func(
            self._compute.get_compute(),
            self._compute_queue,
            ctx,
            arrays_c,
            c_uint32(6),
            c_uint32(resolution),
        )
        if not out:
            raise PipelineError("Failed to build NanoVDB from Gaussian arrays")
        return out.contents

    def nanovdb_from_gaussians_file(
        self,
        filename: str,
        resolution: int = DEFAULT_BVH_RESOLUTION,
    ) -> pnanovdb_ComputeArray:
        """Build a NanoVDB grid directly from a Gaussian splat file (.ply/.npz)."""
        resolution = self._clamp_resolution(resolution)
        ctx = self._ensure_context()

        func = self._voxelbvh.contents.nanovdb_from_gaussians_file
        out = func(
            self._compute.get_compute(),
            self._compute_queue,
            ctx,
            filename.encode("utf-8"),
            c_uint32(resolution),
        )
        if not out:
            raise PipelineError(f"Failed to build NanoVDB from Gaussian file: {filename}")
        return out.contents

    def nanovdb_from_triangles_array(
        self,
        indices: pnanovdb_ComputeArray,
        positions: pnanovdb_ComputeArray,
        colors: pnanovdb_ComputeArray,
        inflation_radius: float = 0.0,
        resolution: int = DEFAULT_BVH_RESOLUTION,
    ) -> pnanovdb_ComputeArray:
        """Build a NanoVDB grid from a triangle mesh.

        Args:
            indices: Flat ``uint32`` triangle indices (3 per triangle).
            positions: Flat ``float32`` vertex positions (3 per vertex).
            colors: Flat ``float32`` per-vertex RGB colors (3 per vertex).
            inflation_radius: World-space inflation of each primitive.
            resolution: VoxelBVH grid resolution (1..4096).
        """
        resolution = self._clamp_resolution(resolution)
        ctx = self._ensure_context()

        func = self._voxelbvh.contents.nanovdb_from_triangles_array
        out = func(
            self._compute.get_compute(),
            self._compute_queue,
            ctx,
            pointer(indices),
            pointer(positions),
            pointer(colors),
            c_float(inflation_radius),
            c_uint32(resolution),
        )
        if not out:
            raise PipelineError("Failed to build NanoVDB from triangle mesh")
        return out.contents

    def nanovdb_from_lines_array(
        self,
        indices: pnanovdb_ComputeArray,
        positions: pnanovdb_ComputeArray,
        colors: pnanovdb_ComputeArray,
        inflation_radius: float = 0.0,
        resolution: int = DEFAULT_BVH_RESOLUTION,
    ) -> pnanovdb_ComputeArray:
        """Build a NanoVDB grid from a set of line segments.

        Args:
            indices: Flat ``uint32`` line indices (2 per segment).
            positions: Flat ``float32`` vertex positions (3 per vertex).
            colors: Flat ``float32`` per-vertex RGB colors (3 per vertex).
            inflation_radius: World-space inflation of each line.
            resolution: VoxelBVH grid resolution (1..4096).
        """
        resolution = self._clamp_resolution(resolution)
        ctx = self._ensure_context()

        func = self._voxelbvh.contents.nanovdb_from_lines_array
        out = func(
            self._compute.get_compute(),
            self._compute_queue,
            ctx,
            pointer(indices),
            pointer(positions),
            pointer(colors),
            c_float(inflation_radius),
            c_uint32(resolution),
        )
        if not out:
            raise PipelineError("Failed to build NanoVDB from line set")
        return out.contents

    def duplicate_topology_array(
        self,
        src: pnanovdb_ComputeArray,
        dst_grid_type: int,
        upsample_factor: int = 1,
    ) -> pnanovdb_ComputeArray:
        """Allocate a new grid that mirrors ``src``'s topology as ``dst_grid_type``.

        This is the pre-allocation step required before an RGBA8 fill: it builds
        a destination grid of the requested type (optionally upsampled) sharing
        the source's node layout.

        Args:
            src: Source VoxelBVH NanoVDB grid.
            dst_grid_type: Destination NanoVDB grid type (e.g.
                ``PNANOVDB_GRID_TYPE_RGBA8``).
            upsample_factor: Topology upsampling factor (>= 1).
        """
        ctx = self._ensure_context()
        dst_ptr = _ComputeArrayPtr()
        func = self._voxelbvh.contents.nanovdb_duplicate_topology_array
        func(
            self._compute.get_compute(),
            self._compute_queue,
            ctx,
            byref(dst_ptr),
            pointer(src),
            c_uint32(int(dst_grid_type)),
            c_uint32(max(1, int(upsample_factor))),
        )
        if not dst_ptr:
            raise PipelineError("Failed to duplicate NanoVDB topology")
        return dst_ptr.contents

    def nanovdb_rgba8_from_array(
        self,
        src: pnanovdb_ComputeArray,
        ray_direction=DEFAULT_RGBA8_RAY_DIRECTION,
        upsample_factor: int = DEFAULT_RGBA8_UPSAMPLE,
    ) -> pnanovdb_ComputeArray:
        """Convert a VoxelBVH NanoVDB grid into an RGBA8 color image grid.

        Bakes per-voxel colors by tracing along ``ray_direction`` in index space,
        producing a NanoVDB grid whose leaf values are packed RGBA8. Internally
        this pre-allocates the destination topology (RGBA8, upsampled) and then
        fills it.

        Args:
            src: Source VoxelBVH NanoVDB grid (from ``nanovdb_from_*``).
            ray_direction: Index-space ray direction ``(x, y, z)`` used to bake
                colors; a zero/invalid direction falls back to ``(0, 0, -1)``.
            upsample_factor: Topology upsampling factor, 1..``MAX_RGBA8_UPSAMPLE``.

        Returns:
            The RGBA8 NanoVDB ``pnanovdb_ComputeArray``.
        """
        upsample_factor = int(upsample_factor)
        if upsample_factor < 1 or upsample_factor > MAX_RGBA8_UPSAMPLE:
            raise InvalidArgumentError(f"upsample_factor must be in [1, {MAX_RGBA8_UPSAMPLE}], got {upsample_factor}")
        direction = tuple(float(v) for v in ray_direction)
        if len(direction) != 3:
            raise InvalidArgumentError(f"ray_direction must have 3 components (x, y, z), got {len(direction)}")
        ctx = self._ensure_context()

        dst = self.duplicate_topology_array(src, PNANOVDB_GRID_TYPE_RGBA8, upsample_factor)

        rx, ry, rz = direction
        func = self._voxelbvh.contents.nanovdb_rgba8_from_voxelbvh_array
        func(
            self._compute.get_compute(),
            self._compute_queue,
            ctx,
            pointer(dst),
            pointer(src),
            pnanovdb_Vec3(float(rx), float(ry), float(rz)),
        )
        return dst

    def nanovdb_rgba8_from_array_directions(
        self,
        src: pnanovdb_ComputeArray,
        directions=None,
        upsample_factor: int = DEFAULT_RGBA8_UPSAMPLE,
    ):
        """Bake an RGBA8 color grid for each ray direction.

        This is the multi-direction counterpart of
        :meth:`nanovdb_rgba8_from_array`: one destination topology is allocated
        and filled per direction. Defaults to :data:`DEFAULT_RGBA8_DIRECTIONS`
        (the same 8 directions the editor uses when
        ``bake_all_directions`` is enabled).

        Args:
            src: Source VoxelBVH NanoVDB grid.
            directions: Iterable of ``(x, y, z)`` index-space ray directions.
                Defaults to :data:`DEFAULT_RGBA8_DIRECTIONS`.
            upsample_factor: Topology upsampling factor, 1..``MAX_RGBA8_UPSAMPLE``.

        Returns:
            A ``list`` of RGBA8 ``pnanovdb_ComputeArray`` grids, one per
            direction (same order as ``directions``).
        """
        if directions is None:
            directions = DEFAULT_RGBA8_DIRECTIONS
        directions = list(directions)
        if not directions:
            raise InvalidArgumentError("directions must contain at least one ray direction")

        results = []
        try:
            for direction in directions:
                results.append(self.nanovdb_rgba8_from_array(src, direction, upsample_factor))
        except Exception:
            # Destroy any grids already produced so a partial failure does not
            # leak native arrays.
            for array in results:
                try:
                    self._compute.destroy_array(array)
                except Exception:
                    pass
            raise
        return results

    def destroy_context(self):
        """Release the build context, if one has been created."""
        if self._context is not None and self._voxelbvh:
            destroy_context = self._voxelbvh.contents.destroy_context
            destroy_context(self._compute.get_compute(), self._compute_queue, self._context)
            self._context = None

    def __del__(self):
        try:
            self.destroy_context()
        except Exception:
            pass
        self._voxelbvh = None
        self._compute = None
