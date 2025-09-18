# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Minimal test to run the editor with streaming enabled

from nanovdb_editor import Compiler, Compute, Editor, Raster, pnanovdb_EditorConfig

if __name__ == "__main__":

    import os
    print(os.getpid())

    compiler = Compiler()
    compiler.create_instance()

    compute = Compute(compiler)
    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    raster = Raster(compute)

    editor = Editor(compute, compiler)

    config = pnanovdb_EditorConfig()
    config.ip_address = b"127.0.0.1"
    config.port = 8080
    config.headless = 1
    config.streaming = 1
    editor.show(config)
