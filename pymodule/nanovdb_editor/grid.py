# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""A friendly wrapper around a NanoVDB grid held in a compute array.

:class:`Grid` owns a native ``pnanovdb_ComputeArray`` and adds Pythonic
conveniences on top of it: it can be used as a context manager (freeing the
underlying array on exit), copied to a NumPy array, or saved to disk. The
high-level :class:`~nanovdb_editor.scene.Scene` helpers return ``Grid`` objects;
the raw array is still available via :attr:`Grid.array` for the lower-level
interfaces.
"""

from __future__ import annotations

from contextlib import contextmanager
from typing import TYPE_CHECKING, Iterator, Optional, Union

import numpy as np

from .compute import Array, pnanovdb_ComputeArray
from .exceptions import InvalidArgumentError

if TYPE_CHECKING:
    from .compute import Compute

# Map a compute array's element size (bytes) to a natural unsigned dtype.
_DTYPE_BY_ELEMENT_SIZE = {1: np.uint8, 2: np.uint16, 4: np.uint32, 8: np.uint64}


class Grid:
    """A NanoVDB grid backed by a native compute array.

    Args:
        compute: The :class:`~nanovdb_editor.Compute` that owns the
            array (used for readback, saving and destruction).
        array: The underlying ``pnanovdb_ComputeArray``.
    """

    def __init__(self, compute: "Compute", array: pnanovdb_ComputeArray):
        self._compute = compute
        self._array: Optional[pnanovdb_ComputeArray] = array

    @staticmethod
    def unwrap(value: Union["Grid", Array, pnanovdb_ComputeArray]) -> pnanovdb_ComputeArray:
        """Return the raw ``pnanovdb_ComputeArray`` for a ``Grid``, :class:`Array`, or array."""
        if isinstance(value, Grid):
            return value.array
        if isinstance(value, Array):
            return value.raw
        return value

    @property
    def array(self) -> pnanovdb_ComputeArray:
        """The underlying ``pnanovdb_ComputeArray`` (raises if closed)."""
        if self._array is None:
            raise InvalidArgumentError("Grid has been closed")
        return self._array

    @property
    def element_count(self) -> int:
        """Number of elements in the grid array."""
        return int(self.array.element_count)

    @property
    def element_size(self) -> int:
        """Size in bytes of a single array element."""
        return int(self.array.element_size)

    def __len__(self) -> int:
        return self.element_count

    def _resolve_dtype(self, dtype: Optional[np.dtype]) -> np.dtype:
        if dtype is None:
            dtype = _DTYPE_BY_ELEMENT_SIZE.get(self.array.element_size, np.uint8)
        return np.dtype(dtype)

    @contextmanager
    def map(self, dtype: Optional[np.dtype] = None) -> Iterator[np.ndarray]:
        """Context manager yielding a live NumPy view of the grid's bytes.

        The view is zero-copy and backed by mapped memory: it is only valid
        inside the ``with`` block, and writes to it update the grid in place.
        Use :meth:`to_numpy` if you want an owned copy instead.

        Args:
            dtype: Element dtype to interpret the array as. Defaults to an
                unsigned integer type matching the array's element size. Its
                item size must equal the array's ``element_size``.

        Example::

            with grid.map(np.uint32) as view:
                view[0] = 0  # writes back to the grid
        """
        with self._compute.mapped_array(self.array, self._resolve_dtype(dtype)) as view:
            yield view

    def to_numpy(self, dtype: Optional[np.dtype] = None) -> np.ndarray:
        """Copy the grid's bytes into a new (owned) NumPy array.

        Args:
            dtype: Element dtype to interpret the array as. Defaults to an
                unsigned integer type matching the array's element size (e.g.
                ``uint32`` for 4-byte elements). Its item size must equal the
                array's ``element_size``.
        """
        with self.map(dtype) as view:
            return np.array(view, copy=True)

    def save(self, filepath: str) -> None:
        """Write the grid to a ``.nvdb`` file."""
        self._compute.save_nanovdb(self.array, filepath)

    def close(self) -> None:
        """Destroy the underlying array. Safe to call more than once."""
        if self._array is not None:
            self._compute.destroy_array(self._array)
            self._array = None

    def __enter__(self) -> "Grid":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __repr__(self) -> str:
        if self._array is None:
            return "Grid(closed)"
        return f"Grid(element_count={self.element_count}, element_size={self.element_size})"
