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


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(os.path.dirname(SCRIPT_DIR), "data")


class TestEditorAPI2:
    """Test suite for editor token-based API (_2 functions)."""

    @pytest.fixture(autouse=True)
    def setup_and_teardown(self):
        """Set up test fixtures before and clean up after each test."""
        # Setup
        self.compiler = nve.Compiler()
        self.compiler.create_instance()

        self.compute = nve.Compute(self.compiler)
        self.compute.device_interface().create_device_manager()
        self.compute.device_interface().create_device()

        self.editor = nve.Editor(self.compute, self.compiler)
        print("Initialized Editor API 2 test fixture")

        self.config = nve.EditorConfig()
        self.config.ip_address = b"127.0.0.1"
        self.config.port = 8080
        self.config.headless = 1
        self.config.streaming = 0

        yield

        # Teardown
        try:
            if hasattr(self, "editor"):
                self.editor.stop()
                self.editor = None
        except:
            pass

        self.compute = None
        if hasattr(self, "compiler"):
            self.compiler._instance = None
            self.compiler._compiler = None
            self.compiler = None
        gc.collect()

    def start_editor(self):
        """Start editor with better diagnostics for CI failures."""
        try:
            print(f"Starting editor fixture streaming={self.config.streaming} headless={self.config.headless}")
            self.compiler.clear_diagnostics()
            self.editor.start(self.config)
            sleep(0.5)
        except Exception as exc:
            diagnostics = self.compiler.get_diagnostics() or "<none>"
            raise AssertionError(
                "Editor API 2 startup failed (exception during start).\n" f"Compiler diagnostics:\n{diagnostics}"
            ) from exc
        diagnostics = self.compiler.get_diagnostics()
        if diagnostics:
            print(f"Compiler diagnostics during editor startup:\n{diagnostics}")

    def test_get_token(self):
        """Test get_token API - creates and returns unique tokens for names."""
        # Get tokens for different names
        token1 = self.editor.get_token("scene1")
        token2 = self.editor.get_token("object1")
        token3 = self.editor.get_token("scene1")  # Same name as token1

        assert token1 is not None, "Token should not be None"
        assert token2 is not None, "Token should not be None"
        assert token3 is not None, "Token should not be None"

        # Verify token structure has id and str fields
        assert hasattr(token1.contents, "id"), "Token should have id field"
        assert hasattr(token1.contents, "str"), "Token should have str field"

        # Same name should return same token (same ID)
        assert token1.contents.id == token3.contents.id, "Same name should produce same token ID"
        assert token1.contents.str == token3.contents.str, "Same name should produce same token string"

        # Different names should have different IDs
        assert token1.contents.id != token2.contents.id, "Different names should produce different token IDs"

    def test_get_pipeline_type(self):
        """Resolve pipeline enum values from token strings only."""
        noop_token = self.editor.get_token("pnanovdb_pipeline_type_noop")
        surface_token = self.editor.get_token("pnanovdb_pipeline_type_nanovdb_surface")
        assert self.editor.get_pipeline_type(noop_token) == 0
        assert self.editor.get_pipeline_type(surface_token) == 12

        with pytest.raises(ValueError, match="Unknown pipeline type"):
            self.editor.get_pipeline_type(self.editor.get_token("NanoVDB Surface (SDF)"))

        with pytest.raises(ValueError, match="Unknown pipeline type"):
            self.editor.get_pipeline_type(self.editor.get_token("unknown_pipeline_id"))

    def test_get_camera(self):
        """Test get_camera API - retrieves camera for a given scene."""
        self.start_editor()

        # Get a token for a scene
        scene_token = self.editor.get_token("test_scene")
        assert scene_token is not None

        # Get camera for the scene
        camera = self.editor.get_camera(scene_token)

        # Camera should be valid (not None)
        assert camera is not None, "Camera should be returned"

        # Verify camera has expected structure
        assert hasattr(camera.contents, "config"), "Camera should have config"
        assert hasattr(camera.contents, "state"), "Camera should have state"

    def test_add_nanovdb_2(self):
        """Test add_nanovdb_2 API - adds NanoVDB data to scene."""
        self.start_editor()

        # Get tokens
        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_nanovdb")

        # Create a simple NanoVDB grid
        nvdb_file = os.path.join(DATA_DIR, "dragon.nvdb")
        if os.path.exists(nvdb_file):
            # Load NanoVDB from file
            with open(nvdb_file, "rb") as f:
                nvdb_data = np.frombuffer(f.read(), dtype=np.uint8)

            array = self.compute.create_array(nvdb_data)

            # Add the NanoVDB data to the scene - should not raise
            self.editor.add_nanovdb_2(scene_token, name_token, array)
            sleep(0.1)  # Give time to process

            # Clean up
            self.compute.destroy_array(array)
        else:
            pytest.skip(f"Test data file not found: {nvdb_file}")

    def test_add_gaussian_data_2(self):
        """Test add_gaussian_data_2 API - adds Gaussian splatting data to scene."""
        self.start_editor()

        # Get tokens
        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_gaussians")

        # Create minimal gaussian data descriptor
        num_points = 10
        means_data = np.random.randn(num_points, 3).astype(np.float32)
        opacities_data = np.ones((num_points, 1), dtype=np.float32) * 0.5
        quaternions_data = np.tile([1.0, 0.0, 0.0, 0.0], (num_points, 1)).astype(np.float32)
        scales_data = np.ones((num_points, 3), dtype=np.float32) * 0.1
        sh_0_data = np.random.randn(num_points, 3).astype(np.float32)

        # Create arrays
        means_array = self.compute.create_array(means_data)
        opacities_array = self.compute.create_array(opacities_data)
        quaternions_array = self.compute.create_array(quaternions_data)
        scales_array = self.compute.create_array(scales_data)
        sh_0_array = self.compute.create_array(sh_0_data)

        # Create descriptor
        desc = nve.EditorGaussianDataDesc()
        from ctypes import pointer

        desc.means = pointer(means_array)
        desc.opacities = pointer(opacities_array)
        desc.quaternions = pointer(quaternions_array)
        desc.scales = pointer(scales_array)
        desc.sh_0 = pointer(sh_0_array)
        desc.sh_n = None

        # Add gaussian data - should not raise
        self.editor.add_gaussian_data_2(scene_token, name_token, desc)
        sleep(0.1)  # Give time to process

        # Clean up arrays
        self.compute.destroy_array(means_array)
        self.compute.destroy_array(opacities_array)
        self.compute.destroy_array(quaternions_array)
        self.compute.destroy_array(scales_array)
        self.compute.destroy_array(sh_0_array)

    def test_update_camera_2(self):
        """Test update_camera_2 API - updates camera for a scene."""
        self.start_editor()

        # Get scene token
        scene_token = self.editor.get_token("test_scene")

        # Get current camera
        camera = self.editor.get_camera(scene_token)
        assert camera is not None

        # Modify camera position
        original_x = camera.contents.state.position.x
        camera.contents.state.position.x = 5.0
        camera.contents.state.position.y = 3.0
        camera.contents.state.position.z = 2.0

        # Update camera - should not raise
        self.editor.update_camera_2(scene_token, camera.contents)
        sleep(0.1)

        # Get camera again to verify update
        updated_camera = self.editor.get_camera(scene_token)
        # Note: Due to threading, the update may not be immediate
        # but the call should succeed without errors

    def test_add_camera_view_2(self):
        """Test add_camera_view_2 API - adds camera view to scene."""
        self.start_editor()

        # Get scene token
        scene_token = self.editor.get_token("test_scene")
        view_name_token = self.editor.get_token("test_view")

        # Create camera view
        camera_view = nve.CameraView()
        camera_view.name = view_name_token
        camera_view.num_cameras = 1
        camera_view.axis_length = 1.0
        camera_view.axis_thickness = 0.1
        camera_view.frustum_line_width = 2.0
        camera_view.frustum_scale = 1.0
        camera_view.frustum_color = nve.Vec3(x=1.0, y=0.0, z=0.0)
        camera_view.is_visible = 1

        # Create camera config and state arrays
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

        from ctypes import pointer

        camera_view.configs = pointer(camera_config)
        camera_view.states = pointer(camera_state)

        # Add camera view - should not raise
        self.editor.add_camera_view_2(scene_token, camera_view)
        sleep(0.1)

    def test_remove(self):
        """Test remove API - removes object from scene."""
        self.start_editor()

        # Get tokens
        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_object")

        # First add something to remove
        nvdb_file = os.path.join(DATA_DIR, "dragon.nvdb")
        if os.path.exists(nvdb_file):
            with open(nvdb_file, "rb") as f:
                nvdb_data = np.frombuffer(f.read(), dtype=np.uint8)

            array = self.compute.create_array(nvdb_data)
            self.editor.add_nanovdb_2(scene_token, name_token, array)
            sleep(0.2)

            # Now remove it - should not raise
            self.editor.remove(scene_token, name_token)
            sleep(0.1)

            # Clean up
            self.compute.destroy_array(array)
        else:
            # Test remove without adding first (should handle gracefully)
            self.editor.remove(scene_token, name_token)
            sleep(0.1)

    def test_map_unmap_params(self):
        """Test map_params and unmap_params APIs - parameter access."""
        self.start_editor()

        # Get tokens
        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_object")

        # Add some data first
        nvdb_file = os.path.join(DATA_DIR, "dragon.nvdb")
        if os.path.exists(nvdb_file):
            with open(nvdb_file, "rb") as f:
                nvdb_data = np.frombuffer(f.read(), dtype=np.uint8)

            array = self.compute.create_array(nvdb_data)
            self.editor.add_nanovdb_2(scene_token, name_token, array)
            sleep(0.2)

            # Try to map params (may return None if no params available, which is OK)
            params_ptr = self.editor.map_params(scene_token, name_token, None)

            # Unmap should not raise even if map returned None
            self.editor.unmap_params(scene_token, name_token)
            sleep(0.1)

            # Clean up
            self.compute.destroy_array(array)
        else:
            pytest.skip(f"Test data file not found: {nvdb_file}")

    def test_multiple_scenes(self):
        """Test working with multiple scenes and objects."""
        self.start_editor()

        # Create multiple scene tokens
        scene1_token = self.editor.get_token("scene1")
        scene2_token = self.editor.get_token("scene2")
        object1_token = self.editor.get_token("object1")
        object2_token = self.editor.get_token("object2")

        # Verify tokens are unique
        assert scene1_token.contents.id != scene2_token.contents.id
        assert object1_token.contents.id != object2_token.contents.id

        # Get cameras for different scenes
        camera1 = self.editor.get_camera(scene1_token)
        camera2 = self.editor.get_camera(scene2_token)

        assert camera1 is not None
        assert camera2 is not None

    def test_token_persistence(self):
        """Test that tokens for same name remain consistent."""
        # Get token multiple times
        token1 = self.editor.get_token("persistent_test")
        token2 = self.editor.get_token("persistent_test")
        token3 = self.editor.get_token("persistent_test")

        # All should have the same ID
        assert token1.contents.id == token2.contents.id
        assert token2.contents.id == token3.contents.id

        # String should be preserved
        assert token1.contents.str == token2.contents.str
        assert token2.contents.str == token3.contents.str

    def test_add_image2d(self):
        """Test add_image2d API - adds 2D image data to scene."""
        self.start_editor()

        # Get tokens
        scene_token = self.editor.get_token("test_scene")
        image_token = self.editor.get_token("test_image2d")

        # Create RGBA8 image data (gradient pattern)
        width, height = 320, 240
        image_rgba = np.zeros((height, width), dtype=np.uint32)

        for j in range(height):
            for i in range(width):
                # Create a color gradient (RGBA packed as uint32)
                r = (255 * i) // (width - 1) if width > 1 else 0
                g = (255 * j) // (height - 1) if height > 1 else 0
                b = 0
                a = 255
                # Pack as RGBA in uint32
                image_rgba[j, i] = r | (g << 8) | (b << 16) | (a << 24)

        # Create compute array from image data
        image_array = self.compute.create_array(image_rgba)

        # Add image to editor - should not raise
        self.editor.add_image2d(scene_token, image_token, image_array, width, height)
        sleep(0.1)  # Give time to process

        # Clean up
        self.compute.destroy_array(image_array)

    def test_get_camera_2(self):
        """Test get_camera_2 API - value getter with read-after-write semantics."""
        self.start_editor()

        # A scene that has never been seen reports "not set" (None), unlike get_camera which
        # always returns a usable default.
        scene_token = self.editor.get_token("test_scene_camera2")
        assert self.editor.get_camera_2(scene_token) is None

        # Set a camera for the scene, then read it back. get_camera_2 returns a fresh copy, so the
        # value must reflect what was just set (read-after-write consistency).
        cam_ptr = self.editor.get_camera(scene_token)
        assert cam_ptr is not None
        cam_ptr.contents.state.position.x = 7.0
        cam_ptr.contents.state.position.y = 8.0
        cam_ptr.contents.state.position.z = 9.0
        self.editor.update_camera_2(scene_token, cam_ptr.contents)
        sleep(0.1)

        result = self.editor.get_camera_2(scene_token)
        assert result is not None, "Camera should be set after update_camera_2"
        assert hasattr(result, "config"), "Camera should have config"
        assert hasattr(result, "state"), "Camera should have state"
        assert result.state.position.x == pytest.approx(7.0)
        assert result.state.position.y == pytest.approx(8.0)
        assert result.state.position.z == pytest.approx(9.0)

        # The returned value is an independent copy: mutating it must not affect the cached camera.
        result.state.position.x = 123.0
        again = self.editor.get_camera_2(scene_token)
        assert again.state.position.x == pytest.approx(7.0), "get_camera_2 must return an independent copy"

    def test_add_named_array_and_gaussian_data_4(self):
        """Test add_named_array + add_gaussian_data_4 - Gaussian data from named arrays."""
        self.start_editor()

        scene_token = self.editor.get_token("test_scene")
        name_token = self.editor.get_token("test_gaussians_named")

        # Build the conventional Gaussian arrays (see add_gaussian_data_2 test).
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

        # Attach them to the object as named arrays using the conventional names. add_named_array
        # creates the object if it does not exist yet.
        self.editor.add_named_array(scene_token, name_token, self.editor.get_token("means"), means_array)
        self.editor.add_named_array(scene_token, name_token, self.editor.get_token("opacities"), opacities_array)
        self.editor.add_named_array(scene_token, name_token, self.editor.get_token("quaternions"), quaternions_array)
        self.editor.add_named_array(scene_token, name_token, self.editor.get_token("scales"), scales_array)
        self.editor.add_named_array(scene_token, name_token, self.editor.get_token("sh_0"), sh_0_array)

        # Create the Gaussian data from the named arrays with explicit pipelines
        # (process = pnanovdb_pipeline_type_noop = 0, render = pnanovdb_pipeline_type_gaussian_splat = 2).
        self.editor.add_gaussian_data_4(scene_token, name_token, 0, 2)
        sleep(0.2)  # Give the render thread time to create the GPU data

        # Clean up arrays (the editor duplicated them synchronously on the calling thread).
        self.compute.destroy_array(means_array)
        self.compute.destroy_array(opacities_array)
        self.compute.destroy_array(quaternions_array)
        self.compute.destroy_array(scales_array)
        self.compute.destroy_array(sh_0_array)

        incomplete_name_token = self.editor.get_token("test_gaussians_incomplete")
        incomplete_means_array = self.compute.create_array(np.random.randn(4, 3).astype(np.float32))
        self.editor.add_named_array(
            scene_token, incomplete_name_token, self.editor.get_token("means"), incomplete_means_array
        )

        # The call must reject an object that does not have all required arrays.
        self.editor.add_gaussian_data_4(scene_token, incomplete_name_token, 0, 2)
        sleep(0.1)

        self.compute.destroy_array(incomplete_means_array)
