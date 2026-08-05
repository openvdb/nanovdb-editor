# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Exception hierarchy for :mod:`nanovdb_editor`.

All library-specific errors derive from :class:`NanoVDBError`, so callers can
catch everything this package raises with a single ``except NanoVDBError``.
The concrete errors also subclass the closest built-in (``RuntimeError`` /
``ValueError``) so existing ``except RuntimeError`` / ``except ValueError``
handlers keep working.
"""


class NanoVDBError(Exception):
    """Base class for all :mod:`nanovdb_editor` errors."""


class DeviceError(NanoVDBError, RuntimeError):
    """A compute device (Vulkan) could not be created or is unavailable."""


class PipelineError(NanoVDBError, RuntimeError):
    """A pipeline, conversion, or native array operation failed."""


class InvalidArgumentError(NanoVDBError, ValueError):
    """An argument was missing, malformed, or out of range."""


class SessionClosedError(NanoVDBError, RuntimeError):
    """A :class:`~nanovdb_editor.Session` was used after :meth:`Session.close`."""
