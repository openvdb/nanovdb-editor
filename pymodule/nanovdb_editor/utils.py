# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import ctypes
import os
import platform
import sys
from ctypes import wintypes
import site


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


def _find_installed_lib_dir(pkg_dir: str) -> str:
    """Locate nanovdb_editor/lib in common install locations.

    Checks the package-local lib, then system and user site-packages,
    then any sys.path entries containing site-packages.
    """
    # 1) package-local lib (editable or wheel install inside repo)
    lib_dir = os.path.join(pkg_dir, "lib")
    if os.path.isdir(lib_dir):
        return lib_dir

    # 2) system and user site-packages
    candidates = []
    for base in list(getattr(site, "getsitepackages", lambda: [])()):
        candidates.append(os.path.join(base, "nanovdb_editor", "lib"))
    user_site = getattr(site, "getusersitepackages", lambda: None)()
    if user_site:
        candidates.append(os.path.join(user_site, "nanovdb_editor", "lib"))

    # 3) any sys.path entry that looks like a site-packages root
    for p in sys.path:
        if p and "site-packages" in p:
            candidates.append(os.path.join(p, "nanovdb_editor", "lib"))

    for d in candidates:
        if os.path.isdir(d):
            return d

    raise OSError("nanovdb_editor lib directory not found in package or site-packages")


def load_library(lib_name) -> ctypes.CDLL:
    system = platform.system()

    package_dir = os.path.dirname(os.path.abspath(__file__))
    lib_dir = _find_installed_lib_dir(package_dir)

    if system == "Windows":
        path = os.path.join(lib_dir, f"{lib_name}.dll")
    elif system == "Linux":
        path = os.path.join(lib_dir, f"lib{lib_name}.so")
    elif system == "Darwin":
        path = os.path.join(lib_dir, f"lib{lib_name}.dylib")
    else:
        raise OSError(f"Unsupported operating system: {system}")

    return ctypes.CDLL(path)
