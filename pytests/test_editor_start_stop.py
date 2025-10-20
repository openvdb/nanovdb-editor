# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import nanovdb_editor as nve  # type: ignore

import os
from time import sleep


def test_editor_start_stop():
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
        editor.start(config)
        # Give the editor a brief moment to initialize
        sleep(0.5)
    finally:
        editor.stop()
        # Prevent destructor from calling into native lib during shutdown
        editor = None
        compute = None
        compiler._instance = None
        compiler._compiler = None
        compiler = None
