# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from concurrent.futures import ThreadPoolExecutor
import ctypes
import importlib.util
import ntpath
import os
from pathlib import PureWindowsPath
import threading
import time
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from nanovdb_editor import utils


@pytest.fixture
def windows_dll_search():
    spec = importlib.util.spec_from_file_location("dll_search_utils", utils.__file__)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    kernel32 = SimpleNamespace(
        SetDefaultDllDirectories=Mock(return_value=1),
        AddDllDirectory=Mock(return_value=123),
    )
    module.sys = SimpleNamespace(platform="win32")
    module.os = SimpleNamespace(
        path=SimpleNamespace(
            exists=Mock(return_value=True),
            abspath=ntpath.abspath,
            normcase=ntpath.normcase,
        ),
        fspath=os.fspath,
    )
    module.ctypes = SimpleNamespace(
        WinDLL=Mock(return_value=kernel32),
        c_void_p=ctypes.c_void_p,
        get_last_error=lambda: 5,
        WinError=lambda code: OSError(code, "DLL registration failed"),
    )
    return module, kernel32


def test_repeated_dll_directory_reuses_registration(windows_dll_search):
    module, kernel32 = windows_dll_search
    paths = [
        r"C:\package\lib",
        "c:/package/./lib",
        r"C:\package\nested\..\lib",
        PureWindowsPath("C:/PACKAGE/lib"),
    ]
    for path in paths * 100:
        module.add_dll_search_directory(path)

    kernel32.AddDllDirectory.assert_called_once_with(r"C:\package\lib")
    kernel32.SetDefaultDllDirectories.assert_called_once_with(0x00001000)


def test_distinct_dll_directories_register_separately(windows_dll_search):
    module, kernel32 = windows_dll_search
    paths = [r"C:\first\lib", r"C:\second\lib"]
    for path in paths * 2:
        module.add_dll_search_directory(path)

    assert [call.args[0] for call in kernel32.AddDllDirectory.call_args_list] == paths


def test_failed_dll_registration_can_retry(windows_dll_search):
    module, kernel32 = windows_dll_search
    kernel32.AddDllDirectory.side_effect = [0, 123, 123]
    with pytest.raises(OSError, match="DLL registration failed") as error:
        module.add_dll_search_directory(r"C:\package\lib")
    assert error.value.errno == 5

    module.add_dll_search_directory(r"C:\package\lib")
    module.add_dll_search_directory(r"C:\package\lib")
    assert kernel32.AddDllDirectory.call_count == 2


def test_missing_dll_directory_can_register_later(windows_dll_search):
    module, kernel32 = windows_dll_search
    module.os.path.exists.side_effect = [False, True]
    module.add_dll_search_directory(r"C:\package\lib")
    kernel32.AddDllDirectory.assert_not_called()

    module.add_dll_search_directory(r"C:\package\lib")
    kernel32.AddDllDirectory.assert_called_once()


def test_concurrent_dll_directory_reuses_registration(windows_dll_search):
    module, kernel32 = windows_dll_search
    callers = 8
    ready = threading.Barrier(callers)

    def register(path):
        time.sleep(0.02)  # The native call releases the GIL.
        return 123

    def add_directory(_):
        ready.wait(timeout=5)
        module.add_dll_search_directory(r"C:\package\lib")

    kernel32.AddDllDirectory.side_effect = register
    with ThreadPoolExecutor(max_workers=callers) as executor:
        list(executor.map(add_directory, range(callers)))

    kernel32.AddDllDirectory.assert_called_once()
