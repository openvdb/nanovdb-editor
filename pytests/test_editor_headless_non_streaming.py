# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import nanovdb_editor as nve
import gc
import os
import platform
from time import sleep


def test_editor_headless_non_streaming():
    """Test editor start in headless non-streaming mode."""

    # Verify we're using software rendering if specified
    if "VK_ICD_FILENAMES" in os.environ or "VK_DRIVER_FILES" in os.environ:
        print(f"Using Vulkan ICD: " f"{os.environ.get('VK_ICD_FILENAMES', 'N/A')}")
        print(f"Using Vulkan driver: " f"{os.environ.get('VK_DRIVER_FILES', 'N/A')}")

    compiler = nve.Compiler()
    compiler.create_instance()

    compute = nve.Compute(compiler)
    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    editor = nve.Editor(compute, compiler)

    config = nve.EditorConfig()
    config.ip_address = b"127.0.0.1"
    config.port = 8080
    config.headless = 1
    config.streaming = 0

    try:
        print(
            f"Starting editor on platform={platform.system()} "
            f"streaming={config.streaming} headless={config.headless}"
        )
        compiler.clear_diagnostics()
        editor.start(config)
        # Give the editor a brief moment to initialize
        sleep(0.5)
        diagnostics = compiler.get_diagnostics()
        if diagnostics:
            print(f"Compiler diagnostics during startup:\n{diagnostics}")
    except Exception as exc:
        raise AssertionError(
            "Headless non-streaming editor start failed.\n"
            f"Compiler diagnostics:\n{compiler.get_diagnostics() or '<none>'}"
        ) from exc
    finally:
        editor.stop()
        # Prevent destructor from calling into native lib during shutdown
        editor = None
        compute = None
        compiler._instance = None  # noqa: SLF001
        compiler._compiler = None  # noqa: SLF001
        compiler = None
        gc.collect()
