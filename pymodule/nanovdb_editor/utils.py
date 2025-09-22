# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import ctypes
import os
import platform
import sys
import glob
from ctypes import wintypes


def add_dll_search_directory(path):
    if sys.platform != "win32":
        return

    if not os.path.exists(path):
        return

    # Enable extended DLL search
    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)

    abs_path = os.path.abspath(path)
    wide_path = os.fspath(abs_path)

    kernel32.AddDllDirectory.argtypes = [wintypes.LPCWSTR]
    kernel32.AddDllDirectory.restype = ctypes.c_void_p

    result = kernel32.AddDllDirectory(wide_path)
    if not result:
        error = ctypes.get_last_error()
        raise ctypes.WinError(error)


def _preload_shared_dependencies(lib_dir: str) -> None:
    """Best-effort preload of dependent shared libraries with RTLD_GLOBAL.

    This helps when transitive dependencies need to resolve symbols across
    multiple modules at runtime (e.g., Slang/Vulkan plugins on Linux).
    """
    system = platform.system()

    if system == "Windows":
        # Windows is handled via add_dll_search_directory in __init__.py
        return

    patterns = []
    if system == "Linux":
        patterns = [
            "libslang*.so*",
            "libvulkan.so*",
        ]
    elif system == "Darwin":
        patterns = [
            "libslang*.dylib",
            "libvulkan*.dylib",
        ]

    for pattern in patterns:
        for candidate in sorted(
            glob.glob(os.path.join(lib_dir, pattern))
        ):
            try:
                ctypes.CDLL(candidate, mode=ctypes.RTLD_GLOBAL)
            except OSError:
                # Ignore missing/failed loads; continue with best-effort
                pass


def load_library(lib_name) -> ctypes.CDLL:
    system = platform.system()

    package_dir = os.path.dirname(os.path.abspath(__file__))
    lib_dir = os.path.join(package_dir, "lib")

    if system == "Windows":
        path = os.path.join(lib_dir, f"{lib_name}.dll")
    elif system == "Linux":
        path = os.path.join(lib_dir, f"lib{lib_name}.so")
    elif system == "Darwin":
        path = os.path.join(lib_dir, f"lib{lib_name}.dylib")
    else:
        raise OSError(f"Unsupported operating system: {system}")

    # Preload transitive dependencies and use RTLD_GLOBAL for symbol visibility
    if system in ("Linux", "Darwin"):
        _preload_shared_dependencies(lib_dir)
        return ctypes.CDLL(path, mode=ctypes.RTLD_GLOBAL)
    else:
        return ctypes.CDLL(path)
