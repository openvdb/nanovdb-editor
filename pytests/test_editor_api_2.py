# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""
Test suite for editor _2 token-based API functions.
"""

import nanovdb_editor as nve  # type: ignore
import os
import gc
import pytest
import numpy as np
from time import sleep
from ctypes import pointer


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(os.path.dirname(SCRIPT_DIR), "data")


@pytest.fixture(scope="class", autouse=True)
def editor_api_2_resources(request):
    """Create one editor for all token-based API tests."""
    compiler = nve.Compiler()
    compiler.create_instance()

    compute = nve.Compute(compiler)
    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    editor = nve.Editor(compute, compiler)
    print("Initialized shared Editor API 2 fixture")

    config = nve.EditorConfig()
    config.ip_address = b"127.0.0.1"
    config.port = 8080
    config.headless = 1
    config.streaming = 0

    request.cls.compiler = compiler
    request.cls.compute = compute
    request.cls.editor = editor
    request.cls.config = config
    request.cls.editor_started = False

    yield

    try:
        editor.stop()
    except Exception:
        pass

    request.cls.editor = None
    request.cls.compute = None
    compiler._instance = None
    compiler._compiler = None
    request.cls.compiler = None
    gc.collect()


class TestEditorAPI2:
    """Test suite for editor token-based API (_2 functions)."""

    def start_editor(self):
        """Start editor once for the class, with diagnostics for CI failures."""
        if self.editor_started:
            return

        try:
            print(f"Starting editor fixture streaming={self.config.streaming} headless={self.config.headless}")
            self.compiler.clear_diagnostics()
            self.editor.start(self.config)
            sleep(0.5)
            self.__class__.editor_started = True
        except Exception as exc:
            diagnostics = self.compiler.get_diagnostics() or "<none>"
            raise AssertionError(
                "Editor API 2 startup failed (exception during start).\n" f"Compiler diagnostics:\n{diagnostics}"
            ) from exc
        diagnostics = self.compiler.get_diagnostics()
        if diagnostics:
            print(f"Compiler diagnostics during editor startup:\n{diagnostics}")

    def test_tokens_and_pipeline_types(self):
        """Token lookup, persistence, and pipeline type resolution (no editor start)."""
        token1 = self.editor.get_token("scene1")
        token2 = self.editor.get_token("object1")
        token3 = self.editor.get_token("scene1")

        assert token1 is not None
        assert token2 is not None
        assert token3 is not None
        assert hasattr(token1.contents, "id")
        assert hasattr(token1.contents, "str")
        assert token1.contents.id == token3.contents.id
        assert token1.contents.str == token3.contents.str
        assert token1.contents.id != token2.contents.id

        persistent1 = self.editor.get_token("persistent_test")
        persistent2 = self.editor.get_token("persistent_test")
        persistent3 = self.editor.get_token("persistent_test")
        assert persistent1.contents.id == persistent2.contents.id == persistent3.contents.id
        assert persistent1.contents.str == persistent2.contents.str == persistent3.contents.str

        noop_token = self.editor.get_token("pnanovdb_pipeline_type_noop")
        surface_token = self.editor.get_token("pnanovdb_pipeline_type_nanovdb_surface")
        assert self.editor.get_pipeline_type(noop_token) == 0
        assert self.editor.get_pipeline_type(surface_token) == 12

        with pytest.raises(ValueError, match="Unknown pipeline type"):
            self.editor.get_pipeline_type(self.editor.get_token("NanoVDB Surface (SDF)"))

        with pytest.raises(ValueError, match="Unknown pipeline type"):
            self.editor.get_pipeline_type(self.editor.get_token("unknown_pipeline_id"))

    def test_camera_and_scenes(self):
        """Camera get/update/view APIs and multi-scene token isolation."""
        self.start_editor()

        scene_token = self.editor.get_token("test_scene")
        camera = self.editor.get_camera(scene_token)
        assert camera is not None
        assert hasattr(camera.contents, "config")
        assert hasattr(camera.contents, "state")

        camera.contents.state.position.x = 5.0
        camera.contents.state.position.y = 3.0
        camera.contents.state.position.z = 2.0
        self.editor.update_camera_2(scene_token, camera.contents)
        sleep(0.1)

        camera2_scene = self.editor.get_token("test_scene_camera2")
        assert self.editor.get_camera_2(camera2_scene) is None

        cam_ptr = self.editor.get_camera(camera2_scene)
        assert cam_ptr is not None
        cam_ptr.contents.state.position.x = 7.0
        cam_ptr.contents.state.position.y = 8.0
        cam_ptr.contents.state.position.z = 9.0
        self.editor.update_camera_2(camera2_scene, cam_ptr.contents)
        sleep(0.1)

        result = self.editor.get_camera_2(camera2_scene)
        assert result is not None
        assert hasattr(result, "config")
        assert hasattr(result, "state")
        assert result.state.position.x == pytest.approx(7.0)
        assert result.state.position.y == pytest.approx(8.0)
        assert result.state.position.z == pytest.approx(9.0)

        result.state.position.x = 123.0
        again = self.editor.get_camera_2(camera2_scene)
        assert again.state.position.x == pytest.approx(7.0), "get_camera_2 must return an independent copy"

        view_name_token = self.editor.get_token("test_view")
        camera_view = nve.CameraView()
        camera_view.name = view_name_token
        camera_view.num_cameras = 1
        camera_view.axis_length = 1.0
        camera_view.axis_thickness = 0.1
        camera_view.frustum_line_width = 2.0
        camera_view.frustum_scale = 1.0
        camera_view.frustum_color = nve.Vec3(x=1.0, y=0.0, z=0.0)
        camera_view.is_visible = 1

        camera_config = nve.CameraConfig()
        camera_config.is_projection_rh = 1
        camera_config.is_orthographic = 0
        camera_config.is_reverse_z = 1
        camera_config.near_plane = 0.1
        camera_config.far_plane = 100.0
        camera_config.fov_angle_y = 45.0
        camera_config.aspect_ratio = 16.0 / 9.0

        camera_state = nve.CameraState()
        camera_state.position = nve.Vec3(x=0.0, y=0.0, z=5.0)
        camera_state.eye_direction = nve.Vec3(x=0.0, y=0.0, z=-1.0)
        camera_state.eye_up = nve.Vec3(x=0.0, y=1.0, z=0.0)
        camera_state.eye_distance_from_position = 5.0
        camera_state.orthographic_scale = 1.0

        camera_view.configs = pointer(camera_config)
        camera_view.states = pointer(camera_state)
        self.editor.add_camera_view_2(scene_token, camera_view)
        sleep(0.1)

        scene1_token = self.editor.get_token("scene1")
        scene2_token = self.editor.get_token("scene2")
        object1_token = self.editor.get_token("object1")
        object2_token = self.editor.get_token("object2")
        assert scene1_token.contents.id != scene2_token.contents.id
        assert object1_token.contents.id != object2_token.contents.id
        assert self.editor.get_camera(scene1_token) is not None
        assert self.editor.get_camera(scene2_token) is not None

    def test_nanovdb_add_remove_and_params(self):
        """add_nanovdb_2, remove, and map/unmap_params on one NanoVDB object."""
        self.start_editor()

        nvdb_file = os.path.join(DATA_DIR, "dragon.nvdb")
        if not os.path.exists(nvdb_file):
            pytest.skip(f"Test data file not found: {nvdb_file}")

        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_nanovdb")

        with open(nvdb_file, "rb") as f:
            nvdb_data = np.frombuffer(f.read(), dtype=np.uint8)

        array = self.compute.create_array(nvdb_data)
        try:
            self.editor.add_nanovdb_2(scene_token, name_token, array)
            sleep(0.2)

            # map may return None if no params are available; unmap must still be safe.
            self.editor.map_params(scene_token, name_token, None)
            self.editor.unmap_params(scene_token, name_token)
            sleep(0.1)

            self.editor.remove(scene_token, name_token)
            sleep(0.1)
        finally:
            self.compute.destroy_array(array)

    def test_gaussian_and_image_apis(self):
        """Gaussian desc/named-array APIs and add_image2d."""
        self.start_editor()

        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_gaussians")

        num_points = 10
        means_data = np.random.randn(num_points, 3).astype(np.float32)
        opacities_data = np.ones((num_points, 1), dtype=np.float32) * 0.5
        quaternions_data = np.tile([1.0, 0.0, 0.0, 0.0], (num_points, 1)).astype(np.float32)
        scales_data = np.ones((num_points, 3), dtype=np.float32) * 0.1
        sh_0_data = np.random.randn(num_points, 3).astype(np.float32)

        means_array = self.compute.create_array(means_data)
        opacities_array = self.compute.create_array(opacities_data)
        quaternions_array = self.compute.create_array(quaternions_data)
        scales_array = self.compute.create_array(scales_data)
        sh_0_array = self.compute.create_array(sh_0_data)

        try:
            desc = nve.EditorGaussianDataDesc()
            desc.means = pointer(means_array)
            desc.opacities = pointer(opacities_array)
            desc.quaternions = pointer(quaternions_array)
            desc.scales = pointer(scales_array)
            desc.sh_0 = pointer(sh_0_array)
            desc.sh_n = None

            self.editor.add_gaussian_data_2(scene_token, name_token, desc)
            sleep(0.1)

            named_token = self.editor.get_token("test_gaussians_named")
            self.editor.add_named_array(scene_token, named_token, self.editor.get_token("means"), means_array)
            self.editor.add_named_array(scene_token, named_token, self.editor.get_token("opacities"), opacities_array)
            self.editor.add_named_array(
                scene_token, named_token, self.editor.get_token("quaternions"), quaternions_array
            )
            self.editor.add_named_array(scene_token, named_token, self.editor.get_token("scales"), scales_array)
            self.editor.add_named_array(scene_token, named_token, self.editor.get_token("sh_0"), sh_0_array)

            # process = noop (0), render = gaussian_splat (2)
            self.editor.add_gaussian_data_4(scene_token, named_token, 0, 2)
            sleep(0.2)

            incomplete_name_token = self.editor.get_token("test_gaussians_incomplete")
            self.editor.add_named_array(
                scene_token, incomplete_name_token, self.editor.get_token("means"), means_array
            )
            # Reject an object that does not have all required arrays.
            self.editor.add_gaussian_data_4(scene_token, incomplete_name_token, 0, 2)
            sleep(0.1)
        finally:
            self.compute.destroy_array(means_array)
            self.compute.destroy_array(opacities_array)
            self.compute.destroy_array(quaternions_array)
            self.compute.destroy_array(scales_array)
            self.compute.destroy_array(sh_0_array)

        image_token = self.editor.get_token("test_image2d")
        width, height = 320, 240
        image_rgba = np.zeros((height, width), dtype=np.uint32)
        for j in range(height):
            for i in range(width):
                r = (255 * i) // (width - 1) if width > 1 else 0
                g = (255 * j) // (height - 1) if height > 1 else 0
                image_rgba[j, i] = r | (g << 8) | (0 << 16) | (255 << 24)

        image_array = self.compute.create_array(image_rgba)
        try:
            self.editor.add_image2d(scene_token, image_token, image_array, width, height)
            sleep(0.1)
        finally:
            self.compute.destroy_array(image_array)
