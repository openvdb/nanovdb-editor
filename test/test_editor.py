# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import nanovdb_editor as nve

import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_NANOVDB = os.path.join(SCRIPT_DIR, "../data/dragon.nvdb")

if __name__ == "__main__":

    with nve.create_default() as app:
        with app.scene("main").nanovdb_from_file(TEST_NANOVDB, name="dragon") as dragon:
            print(f"Loaded dragon grid: {dragon.element_count} elements")

        config = nve.EditorConfig()
        config.ip_address = b"127.0.0.1"
        config.port = 8080
        config.headless = 0
        config.streaming = 0
        app.show(config)
