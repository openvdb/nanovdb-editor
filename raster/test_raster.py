# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import numpy as np

import nanovdb_editor as nve


TEST_RASTER_TO_NANOVDB = True

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_NPZ = os.path.join(SCRIPT_DIR, "../data/splats.npz")
TEST_NANOVDB = os.path.join(SCRIPT_DIR, "../data/raster_test.nvdb")

VOXEL_SIZE = 1.0 / 128.0

if __name__ == "__main__":

    print(f"Current Process ID (PID): {os.getpid()}")

    with nve.create_default() as app:

        def raster_func(filename_ptr):
            if isinstance(filename_ptr, bytes):
                _filename = filename_ptr.decode("utf-8")
            else:
                _filename = filename_ptr

            try:
                npz_array = np.load(TEST_NPZ)
            except FileNotFoundError:
                print(f"File '{_filename}' not found")
                return None

            print(f"Rasterizing npz file '{_filename}'...")

            # Feed the raw splat parameters straight to the pipeline: quaternion
            # normalization, scale exp, color-from-SH and the opacity sigmoid are
            # all applied inside the "raster3d" process.
            means = npz_array["means"]
            opacities = npz_array["opacities"].reshape(-1, 1)
            quaternions = npz_array["quaternions"]
            scales = npz_array["scales"]
            sh = npz_array["sh"]  # (N, 16, 3): order-0 coeff + 15 higher-order RGB

            sh_0 = sh[:, 0, :]
            sh_n = sh[:, 1:, :].reshape(sh.shape[0], -1)

            # Build a NanoVDB grid and register it with the "main" scene as "splats".
            return app.scene("main").nanovdb_from_gaussians(
                means=means,
                quats=quaternions,
                scales=scales,
                sh_0=sh_0,
                sh_n=sh_n,
                opacities=opacities,
                process="raster3d",
                voxel_size=VOXEL_SIZE,
                name="splats",
            )

        if TEST_RASTER_TO_NANOVDB:
            grid = raster_func(TEST_NPZ)
            if grid is not None:
                # Grid is a context manager: the native array is freed on block exit
                # (the editor keeps its own copy of the registered grid).
                with grid:
                    # grid.map() yields a live, zero-copy view of the grid's bytes
                    # and unmaps it automatically at the end of the block.
                    with grid.map() as view:
                        print(f"NanoVDB grid: {len(view)} elements, first byte {int(view[0])}")
                    grid.save(TEST_NANOVDB)

        app.editor.add_callable("Raster", raster_func)
        app.show()
    # Session closes here: worker stopped, editor shut down, compiler destroyed.
