# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import pytest
import sys
import os


def cleanup_modules(exit_code=0):
    """Remove native library wrapper modules to prevent segfaults during interpreter shutdown"""
    try:
        # Remove computed/native lib wrapper modules from sys.modules
        # This prevents ctypes library destructor issues during interpreter shutdown
        # NOTE: Do NOT call gc.collect() here - it triggers native object destructors
        # while threads/state may still be active, causing memory corruption
        modules_to_remove = [
            "nanovdb_editor",
            "nanovdb_editor.compiler",
            "nanovdb_editor.compute",
            "nanovdb_editor.device",
            "nanovdb_editor.editor",
            "nanovdb_editor.raster",
            "nanovdb_editor.session",
            "nanovdb_editor.utils",
        ]

        for module_name in modules_to_remove:
            if module_name in sys.modules:
                del sys.modules[module_name]

        # Force exit to prevent library unload segfaults
        # Preserve the original exit code from pytest
        os._exit(exit_code)
    except Exception:
        # If cleanup fails, still try to exit cleanly
        os._exit(1)


@pytest.hookimpl(hookwrapper=True, tryfirst=True)
def pytest_sessionfinish(session, exitstatus):
    outcome = yield
    if outcome.excinfo is not None:
        # Let pytest report hook failures and select the exit status.
        return
    sys.stdout.flush()
    sys.stderr.flush()
    cleanup_modules(session.exitstatus)
