# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

#!/usr/bin/env python3

# Equivalent to https://github.com/openvdb/fvdb-core/blob/main/tests/unit/test_viz.py
# The file hasn't existed at the time of the release of fvdb-core 0.3.0
# TODO: Fetch from the upstream repository in new fvdb-core releases

import os
import sys
import unittest
import warnings

import pytest

fvdb = pytest.importorskip("fvdb", reason="fvdb package is required for fvdb viz tests")
np = pytest.importorskip("numpy", reason="numpy is required for fvdb viz tests")
torch = pytest.importorskip("torch", reason="torch is required for fvdb viz tests")

PORT = 8080


class TestViewerServer(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        try:
            fvdb.viz.init(ip_address="127.0.0.1", port=PORT, verbose=False)
        except Exception as exc:  # pragma: no cover - upstream parity
            pytest.skip(f"Could not initialize viewer server: {exc}")

    def test_init(self):
        assert fvdb.viz._viewer_server._viewer_server_cpp is not None

        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            fvdb.viz.init(ip_address="127.0.0.1", port=PORT)

            assert len(caught) == 1
            assert "already initialized" in str(caught[0].message)

    def test_show(self):
        fvdb.viz.show()


class TestViewerScene(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        try:
            if fvdb.viz._viewer_server._viewer_server_cpp is None:
                fvdb.viz.init(ip_address="127.0.0.1", port=PORT, verbose=False)
        except Exception as exc:  # pragma: no cover - upstream parity
            pytest.skip(f"Could not initialize viewer server: {exc}")

    def test_scene_creation(self):
        scene = fvdb.viz.Scene("test_scene_creation")
        assert scene._name == "test_scene_creation"

    def test_get_scene(self):
        scene = fvdb.viz.get_scene("test_get_scene")
        assert scene._name == "test_get_scene"

    def test_add_point_cloud(self):
        scene = fvdb.viz.Scene("test_point_cloud")

        points = np.random.randn(100, 3)
        colors = np.random.rand(100, 3)
        point_size = 2.0

        view = scene.add_point_cloud("test_pc", points, colors, point_size)
        assert view is not None

    def test_add_cameras(self):
        scene = fvdb.viz.Scene("test_cameras")

        num_cameras = 3
        camera_to_world = torch.eye(4).unsqueeze(0).repeat(num_cameras, 1, 1)
        projection = torch.eye(3).unsqueeze(0).repeat(num_cameras, 1, 1)

        view = scene.add_cameras("test_cams", camera_to_world, projection)
        assert view is not None

    def test_scene_reset(self):
        scene = fvdb.viz.Scene("test_reset")
        scene.reset()

    def test_multiple_scenes(self):
        scene1 = fvdb.viz.get_scene("Scene 1")
        scene2 = fvdb.viz.get_scene("Scene 2")

        points = torch.randn(20, 3)
        colors = torch.rand(20, 3)

        scene1.add_point_cloud("pc1", points, colors, 2.0)
        scene2.add_point_cloud("pc2", points * 2, colors, 2.0)

        assert scene1._name == "Scene 1"
        assert scene2._name == "Scene 2"

    # def test_add_image(self):
    #     scene = fvdb.viz.Scene("test_image")

    #     width = 64
    #     height = 64

    #     rgba_data = np.zeros((height, width, 4), dtype=np.uint8)
    #     rgba_data[:, :, 0] = 255
    #     rgba_data[:, :, 1] = 128
    #     rgba_data[:, :, 2] = 0
    #     rgba_data[:, :, 3] = 255

    #     rgba_flat = rgba_data.reshape(-1)

    #     view = scene.add_image("small_image", rgba_flat, width, height)
    #     assert view is not None
    #     assert isinstance(view, fvdb.viz.ImageView)
    #     assert view.name == "small_image"
    #     assert view.scene_name == "test_image"
    #     assert view.width == width
    #     assert view.height == height

    #     rgba_data2 = np.zeros((height, width, 4), dtype=np.uint8)
    #     rgba_data2[:, :, 2] = 255
    #     rgba_data2[:, :, 3] = 255
    #     rgba_flat2 = rgba_data2.reshape(-1)
    #     view.update(rgba_flat2)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
