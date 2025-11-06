# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from nanovdb_editor import Compiler, Compute, Editor, EditorConfig

import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_NANOVDB = os.path.join(SCRIPT_DIR, "../data/dragon_node2.nvdb")

if __name__ == "__main__":

    compiler = Compiler()
    compiler.create_instance()

    compute = Compute(compiler)
    nvdb_array = compute.load_nanovdb(TEST_NANOVDB)

    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    editor = Editor(compute, compiler)

    scene_token = editor.get_token("main")
    dragon_token = editor.get_token("dragon")
    editor.add_nanovdb_2(scene_token, dragon_token, nvdb_array)

    config = EditorConfig()
    config.ip_address = b"127.0.0.1"
    config.port = 8080
    config.headless = 0
    config.streaming = 0
    editor.show(config)

    compute.destroy_array(nvdb_array)
