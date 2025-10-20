# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import pytest
import sys
import gc
import atexit
import os


def cleanup_modules():
    """Remove native library wrapper modules to prevent segfaults during interpreter shutdown"""
    try:
        # Force garbage collection to clear any remaining objects
        gc.collect()

        # Remove computed/native lib wrapper modules from sys.modules
        # This prevents ctypes library destructor issues during interpreter shutdown
        modules_to_remove = [
            'nanovdb_editor',
            'nanovdb_editor.compiler',
            'nanovdb_editor.compute',
            'nanovdb_editor.device',
            'nanovdb_editor.editor',
            'nanovdb_editor.raster',
            'nanovdb_editor.utils',
        ]

        for module_name in modules_to_remove:
            if module_name in sys.modules:
                del sys.modules[module_name]

        # Final garbage collection pass
        gc.collect()

        # Force exit to prevent library unload segfaults
        os._exit(0)
    except Exception:
        # If cleanup fails, still try to exit cleanly
        os._exit(1)


def pytest_sessionfinish(session, exitstatus):
    """Hook called after session finishes - use it to trigger exit"""
    # Schedule cleanup to happen immediately
    cleanup_modules()


# Register cleanup at module exit
atexit.register(cleanup_modules)
