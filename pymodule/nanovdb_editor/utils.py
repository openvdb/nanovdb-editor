# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import ctypes
import glob
import importlib.util
import os
import platform
import site
import sys
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


_LOADED_SHARED_LIBRARIES = {}


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


def _try_find_installed_lib_dir(pkg_dir: str):
    try:
        return _find_installed_lib_dir(pkg_dir)
    except OSError:
        return None


def _find_package_dir(package_name: str):
    spec = importlib.util.find_spec(package_name)
    if spec and spec.origin:
        return os.path.dirname(os.path.abspath(spec.origin))
    return None


def _shared_library_name(stem: str) -> str:
    system = platform.system()
    if system == "Windows":
        return f"{stem}.dll"
    if system == "Linux":
        return f"lib{stem}.so"
    if system == "Darwin":
        return f"lib{stem}.dylib"
    raise OSError(f"Unsupported operating system: {system}")


def _shared_library_patterns(stem: str):
    system = platform.system()
    if system == "Windows":
        return [f"{stem}.dll"]
    if system == "Linux":
        return [f"lib{stem}.so", f"lib{stem}.so.*"]
    if system == "Darwin":
        return [f"lib{stem}.dylib", f"lib{stem}.*.dylib"]
    raise OSError(f"Unsupported operating system: {system}")


def _glob_first(paths):
    for pattern in paths:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def _resolve_library_in_dirs(search_dirs, stem: str):
    for directory in search_dirs:
        if not directory or not os.path.isdir(directory):
            continue
        patterns = [os.path.join(directory, pattern) for pattern in _shared_library_patterns(stem)]
        match = _glob_first(patterns)
        if match:
            return match
    return None


def _resolve_library_from_env(stem: str):
    for env_name in ("NANOVDB_EDITOR_SLANG_LLVM_PATH", "SLANG_LLVM_PATH"):
        raw_value = os.environ.get(env_name)
        if not raw_value:
            continue

        candidate = os.path.abspath(os.path.expanduser(raw_value))
        if os.path.isdir(candidate):
            candidate = os.path.join(candidate, _shared_library_name(stem))

        if os.path.exists(candidate):
            return candidate
    return None


def _load_shared_library(path: str, *, global_symbols: bool = False):
    if not path or not os.path.exists(path):
        return None

    cached = _LOADED_SHARED_LIBRARIES.get(path)
    if cached is not None:
        return cached

    if platform.system() == "Windows":
        lib = ctypes.CDLL(path)
    else:
        if global_symbols:
            mode = getattr(ctypes, "RTLD_GLOBAL", 0)
        else:
            mode = getattr(ctypes, "RTLD_LOCAL", 0)
        lib = ctypes.CDLL(path, mode=mode)

    _LOADED_SHARED_LIBRARIES[path] = lib
    return lib


def _slang_runtime_search_dirs(lib_dir: str):
    search_dirs = []
    slangpy_dir = _find_package_dir("slangpy")
    if slangpy_dir:
        search_dirs.append(slangpy_dir)
    if lib_dir:
        search_dirs.append(lib_dir)
    return search_dirs


def has_slang_llvm_runtime() -> bool:
    package_dir = os.path.dirname(os.path.abspath(__file__))
    lib_dir = _try_find_installed_lib_dir(package_dir)
    search_dirs = _slang_runtime_search_dirs(lib_dir)
    return bool(_resolve_library_from_env("slang-llvm") or _resolve_library_in_dirs(search_dirs, "slang-llvm"))


def _configure_slang_runtime(lib_dir: str) -> None:
    search_dirs = _slang_runtime_search_dirs(lib_dir)

    if platform.system() == "Windows":
        for directory in search_dirs:
            if directory and os.path.isdir(directory):
                add_dll_search_directory(directory)

    compiler_runtime = _resolve_library_in_dirs(search_dirs, "slang-compiler")
    if compiler_runtime:
        _load_shared_library(compiler_runtime, global_symbols=True)

    llvm_runtime = _resolve_library_from_env("slang-llvm")
    if not llvm_runtime:
        llvm_runtime = _resolve_library_in_dirs(search_dirs, "slang-llvm")
    if llvm_runtime:
        llvm_dir = os.path.dirname(llvm_runtime)
        if platform.system() == "Windows" and os.path.isdir(llvm_dir):
            add_dll_search_directory(llvm_dir)
        _load_shared_library(llvm_runtime, global_symbols=True)


def load_library(lib_name) -> ctypes.CDLL:
    system = platform.system()

    package_dir = os.path.dirname(os.path.abspath(__file__))
    lib_dir = _find_installed_lib_dir(package_dir)
    _configure_slang_runtime(lib_dir)

    if system == "Windows":
        add_dll_search_directory(lib_dir)
        path = os.path.join(lib_dir, f"{lib_name}.dll")
    elif system == "Linux":
        path = os.path.join(lib_dir, f"lib{lib_name}.so")
    elif system == "Darwin":
        path = os.path.join(lib_dir, f"lib{lib_name}.dylib")
    else:
        raise OSError(f"Unsupported operating system: {system}")

    return ctypes.CDLL(path)
