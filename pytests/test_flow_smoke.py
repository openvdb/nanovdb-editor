# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import ctypes
import json
import struct
from pathlib import Path
import unittest
from tempfile import TemporaryDirectory

import numpy as np
import pytest

from nanovdb_editor import Compiler, Compute, CompileTarget, MemoryBuffer

from test_dispatch import cpu_target_supported


ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / "editor/shaders/flow_smoke.slang"
MATERIAL_TEST = ROOT / "pytests/shaders/test_flow_smoke.slang"
RENDER_TEST = ROOT / "pytests/shaders/test_flow_smoke_render.slang"


class Material(ctypes.Structure):
    _fields_ = [
        ("attenuation", ctypes.c_float),
        ("step_size_scale", ctypes.c_float),
        ("color_scale", ctypes.c_float),
        ("fallback_temperature", ctypes.c_float),
        ("colormap_min", ctypes.c_float),
        ("colormap_max", ctypes.c_float),
        ("colormap_resolution", ctypes.c_uint32),
        ("point_count", ctypes.c_uint32),
        ("point_positions0", ctypes.c_float * 4),
        ("point_positions1", ctypes.c_float * 4),
        *[(f"point_color{index}", ctypes.c_float * 4) for index in range(8)],
        ("shadow_direction", ctypes.c_float * 3),
        ("shadow_factor", ctypes.c_float),
        ("shadow_attenuation", ctypes.c_float),
        ("shadow_step_size_scale", ctypes.c_float),
        ("shadow_step_offset_scale", ctypes.c_float),
        ("shadow_min_intensity", ctypes.c_float),
        ("shadow_num_steps", ctypes.c_uint32),
        ("shadow_cell_size_scale", ctypes.c_float),
        ("shadow_colormap_range", ctypes.c_float * 2),
    ]


class UniformState(ctypes.Structure):
    _fields_ = [
        ("data_in", MemoryBuffer),
        ("constants", ctypes.c_void_p),
        ("data_out", MemoryBuffer),
    ]


class PhysicalDeviceDesc(ctypes.Structure):
    _fields_ = [
        ("device_name", ctypes.c_char * 256),
        ("device_uuid", ctypes.c_uint8 * 16),
        ("device_luid", ctypes.c_uint8 * 8),
        ("device_node_mask", ctypes.c_uint32),
        ("device_luid_valid", ctypes.c_int32),
    ]


class FlowSmokeTestCase(unittest.TestCase):
    compile_target = CompileTarget.VULKAN

    def setUp(self):
        self.compiler = Compiler()
        self.compiler.create_instance()
        self.addCleanup(self.compiler.destroy_instance)
        self.compute = None
        self.material = Material()
        defaults = json.loads(SHADER.with_suffix(".slang.json").read_text())["ShaderParams"]
        for name, field_type in Material._fields_:
            value = defaults[name]["value"]
            setattr(self.material, name, field_type(*value) if isinstance(value, list) else value)

    def get_compute(self):
        if self.compute is None:
            self.compute = Compute(self.compiler)
            device_interface = self.compute.device_interface()
            native = device_interface.get_device_interface().contents
            device_interface.create_device_manager()
            self.addCleanup(native.destroy_device_manager, device_interface._device_manager)
            device = device_interface.create_device()
            self.addCleanup(native.destroy_device, device_interface._device_manager, device)
        return self.compute


class TestFlowSmoke(FlowSmokeTestCase):
    def setUp(self):
        super().setUp()
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.material_shader = Path(directory.name) / "material_test.slang"
        self.material_shader.write_text(MATERIAL_TEST.read_text().replace(
            '"flow_smoke_material.slang"', f'"{(SHADER.parent / "flow_smoke_material.slang").as_posix()}"'))

    def run_material(self, samples, entry_point="computeMain"):
        source = np.asarray(samples, dtype=np.float32)
        result = np.zeros_like(source)
        if self.compile_target == CompileTarget.CPU:
            success = self.compiler.compile_shader(
                str(self.material_shader), entry_point_name=entry_point, compile_target=self.compile_target
            )
            self.assertTrue(success, self.compiler.get_diagnostics())
            uniforms = UniformState(
                MemoryBuffer(source), ctypes.addressof(self.material), MemoryBuffer(result)
            )
            success = self.compiler.execute_cpu(
                str(self.material_shader), (len(samples), 1, 1), None, ctypes.addressof(uniforms)
            )
            self.assertTrue(success, self.compiler.get_diagnostics())
            return result

        compute = self.get_compute()
        with TemporaryDirectory() as directory:
            shader = Path(directory) / "material.slang"
            shader.write_text(
                f'#include "{self.material_shader.as_posix()}"\n'
                '[shader("compute")][numthreads(1, 1, 1)]\n'
                f'void main(uint3 thread_id : SV_DispatchThreadID) {{ {entry_point}(thread_id); }}\n'
            )
            self.assertTrue(self.compiler.compile_shader(str(shader)), self.compiler.get_diagnostics())
            with compute.array(source) as data_in, \
                    compute.array(np.frombuffer(self.material, dtype=np.uint32)) as constants, \
                    compute.array(result) as data_out:
                success = compute.dispatch_shader_on_array(
                    str(shader), (len(samples), 1, 1), data_in.raw, constants.raw, data_out.raw
                )
                self.assertTrue(success, self.compiler.get_diagnostics())
                with compute.mapped_array(data_out.raw, source.dtype) as mapped:
                    return mapped.copy().reshape(source.shape)

    def test_renderer_compiles(self):
        with TemporaryDirectory() as directory:
            shader = Path(directory) / "renderer.slang"
            shader.write_text(f'#include "{SHADER.as_posix()}"\n')
            self.assertTrue(self.compiler.compile_shader(str(shader)), self.compiler.get_diagnostics())

    def test_distant_camera_steps_advance_and_cover_the_interval(self):
        near = np.float32(1e6)
        requested_step = np.float32(0.01) * np.float32(0.75)
        self.assertEqual(np.float32(near + requested_step), near)
        samples = [[near, near + 0.5, requested_step, 0.37], [near, near + 100, requested_step, 0.37]]
        result = self.run_material(samples, entry_point="computeRayStepsMain")
        self.assertEqual(result[0, 2], requested_step)
        self.assertEqual(result[1, 0], 4096)
        for (count, advanced, step, last), (_, far, _, _) in zip(result, samples):
            self.assertGreater(count, 0)
            self.assertEqual(advanced, count)
            self.assertGreaterEqual(last + step, far - near)
            self.assertLess(last, far - near)

    def test_ray_steps_preserve_regular_spacing_and_jitter(self):
        samples = [[0, 10, 0.75, 0.2], [3.25, 15, 0.75, 0.4], [1, 4, 0.5, 0]]
        result = self.run_material(samples, entry_point="computeRayStepsMain")
        for actual, (near, far, step, noise) in zip(result, samples):
            first = (np.ceil(near / step + noise) - noise) * step
            count = np.ceil((far - first) / step)
            np.testing.assert_allclose(actual, [count, count, step, first - near + (count - 1) * step], atol=1e-6)

    def test_extreme_ray_lengths_have_bounded_work(self):
        samples = [[0, 1e30, 1e-20, 0.5], [1e6, 1e6 + 1, 1e-12, 0.5]]
        result = self.run_material(samples, entry_point="computeRayStepsMain")
        np.testing.assert_array_equal(result[:, :2], [[4096, 4096], [4096, 4096]])
        for actual, (near, far, _, _) in zip(result, samples):
            count, _, step, last = actual
            self.assertTrue(np.isfinite(actual).all())
            self.assertLess(last, far - near)
            self.assertGreaterEqual(last + step, np.float32(far - near))

    def test_invalid_ray_intervals_do_not_march(self):
        samples = [
            [0, 0, 1, 0], [1, 0, 1, 0], [0, 1, 0, 0], [0, 1, -1, 0],
            [0, np.inf, 1, 0], [np.nan, 1, 1, 0], [0, 1, np.inf, 0], [0, 1, 1, np.nan],
        ]
        result = self.run_material(samples, entry_point="computeRayStepsMain")
        np.testing.assert_array_equal(result, [[0, 0, 0, -1]] * len(samples))

    def test_shadow_steps_are_bounded_even_for_uint32_max(self):
        for requested in (0, 16, 128, 129, 2**32 - 1):
            with self.subTest(requested=requested):
                self.material.shadow_num_steps = requested
                result = self.run_material([[0, 0, 0, 0]], entry_point="computeShadowCountMain")[0]
                count = min(requested, 128)
                self.assertEqual(result[0], count)
                self.assertAlmostEqual(result[1], np.float32(0.99)**count, places=6)

    def test_total_shadow_samples_per_ray_are_bounded(self):
        lengths = [0, 1, 31, 32, 33, 64, 255, 256, 257, 1000, 4095, 4096]
        for requested in (0, 1, 16, 128, 2**32 - 1):
            with self.subTest(requested=requested):
                self.material.shadow_num_steps = requested
                result = self.run_material([[n, 0, 0, 0] for n in lengths],
                                           entry_point="computeShadowBudgetMain")
                self.assertTrue((result[:, 0] <= 4096).all())
                for length, (_, stride, _, _) in zip(lengths, result):
                    if length * min(requested, 128) <= 4096:
                        self.assertEqual(stride, 1)

    def test_flow_opacity_and_front_to_back_compositing(self):
        self.material.point_count = 2
        self.material.point_positions0[:] = [0, 1, 1, 1]
        self.material.point_color0[:] = [0.2, 0.4, 0.8, 0.75]
        self.material.point_color1[:] = self.material.point_color0[:]
        self.material.color_scale = 2
        self.material.attenuation = 0.4
        samples = [[0.5, 0.3, 0.75, 4], [0.5, 9, 0.75, 4], [0.5, 0, 0.75, 4]]
        result = self.run_material(samples)
        for actual, (_, smoke, step, count) in zip(result, samples):
            alpha = min(0.75 * smoke, 1) * (1 - np.exp(-0.4 * step))
            transmittance = (1 - alpha) ** count
            lookup_rgb = np.asarray([0.2, 0.4, 0.8], dtype=np.float16).astype(np.float32)
            expected = [*(2 * lookup_rgb * (1 - transmittance)), transmittance]
            np.testing.assert_allclose(actual, expected, atol=1e-6)

    def test_colormap_texels_preserve_half_precision(self):
        samples = np.asarray([
            [0, -0.0, 2**-25, 3 * 2**-25],
            [2**-24, -2**-24, 2**-14, 2**-14 - 2**-24],
            [0.2, 0.4, 0.8, 0.904902],
            [65504, -65504, np.inf, -np.inf],
        ], dtype=np.float32)
        result = self.run_material(samples, entry_point="computeColormapTexelMain")
        expected = samples.astype(np.float16).astype(np.float32)
        np.testing.assert_array_equal(result.view(np.uint32), expected.view(np.uint32))

    def test_temperature_uses_filtered_colormap(self):
        self.material.point_count = 3
        self.material.point_positions0[:] = [0, 0.5, 1, 1]
        self.material.point_color0[:] = [1, 0, 0, 1]
        self.material.point_color1[:] = [0, 1, 0, 1]
        self.material.point_color2[:] = [0, 0, 1, 1]
        self.material.colormap_resolution = 4
        self.material.color_scale = 1
        self.material.attenuation = 1
        result = self.run_material([[0, 1, 1, 1], [0.5, 1, 1, 1], [1, 1, 1, 1]])
        alpha = 1 - np.exp(-1)
        expected_rgb = np.asarray([[0.75, 0.25, 0], [0.125, 0.75, 0.125], [0, 0.25, 0.75]])
        np.testing.assert_allclose(result[:, :3], alpha * expected_rgb, atol=1e-6)
        np.testing.assert_allclose(result[:, 3], 1 - alpha, atol=1e-6)

    def test_native_smoke_colormap_has_no_middle_opacity_dip(self):
        self.material.color_scale = 1
        self.material.attenuation = 1
        result = self.run_material([[0.6, 1, 1, 1], [0.85, 1, 1, 1], [1, 1, 1, 1]])
        alpha = float(np.float16(0.904902)) * (1 - np.exp(-1))
        np.testing.assert_allclose(result[:, 3], 1 - alpha, atol=1e-6)

    def test_native_shadow_attenuation_and_minimum_intensity(self):
        samples = [[0.4, 0.3, 0.12, 16], [0.9, 3, 0.12, 16], [0.9, 0, 0.12, 16]]
        result = self.run_material(samples, entry_point="computeShadowMain")
        for actual, (alpha, smoke, step, count) in zip(result, samples):
            opacity = min(alpha * smoke, 1) * (1 - np.exp(-0.045 * step))
            transmittance = (1 - opacity) ** count
            intensity = 0.125 + 0.875 * transmittance
            np.testing.assert_allclose(actual, [intensity] * 3 + [transmittance], atol=1e-6)

    def test_shadow_factor_zero_disables_darkening(self):
        self.material.shadow_factor = 0
        result = self.run_material([[1, 10, 2, 16]], entry_point="computeShadowMain")
        np.testing.assert_array_equal(result[:, :3], [[1, 1, 1]])
        self.assertLess(result[0, 3], 1)

    def test_shadow_intensity_does_not_fall_below_native_floor(self):
        self.material.shadow_attenuation = 1
        result = self.run_material([[1, 10, 10, 16]], entry_point="computeShadowMain")
        np.testing.assert_allclose(result[:, :3], [[0.125] * 3], atol=1e-6)

    def test_zero_attenuation_is_transparent(self):
        self.material.attenuation = 0
        result = self.run_material([[0.2, 100, 1, 10], [0.8, 100, 1, 10]])
        np.testing.assert_array_equal(result, [[0, 0, 0, 1], [0, 0, 0, 1]])


@unittest.skipUnless(cpu_target_supported(), "CPU shader target is unavailable on Linux and Windows ARM64")
class TestFlowSmokeCPU(TestFlowSmoke):
    compile_target = CompileTarget.CPU


class TestFlowSmokeRender(FlowSmokeTestCase):
    def setUp(self):
        super().setUp()
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.shader = Path(directory.name) / "render.slang"
        source = RENDER_TEST.read_text()
        for name in ("flow_smoke_material.slang", "flow_smoke_ray_march.slang"):
            source = source.replace(f'"{name}"', f'"{(SHADER.parent / name).as_posix()}"')
        self.shader.write_text(source)
        self.material.shadow_direction[:] = [1, 0, 0]
        self.material.fallback_temperature = 0.5
        self.material.step_size_scale = 64

    @staticmethod
    def constant_grid(value, voxel_size=1 / 1024):
        # One active root tile covers [0, 4096)^3; values outside the tile are zero.
        data = bytearray(832)
        data[:8] = b"NanoVDB1"
        struct.pack_into("<I", data, 16, 32 << 21)
        struct.pack_into("<I", data, 28, 1)
        struct.pack_into("<Q", data, 32, len(data))
        for offset, fmt, diagonal in ((296, "<9f", voxel_size), (332, "<9f", 1 / voxel_size),
                                     (384, "<9d", voxel_size), (456, "<9d", 1 / voxel_size)):
            struct.pack_into(fmt, data, offset, diagonal, 0, 0, 0, diagonal, 0, 0, 0, diagonal)
        struct.pack_into("<6d", data, 560, 0, 0, 0, *(3 * [4096 * voxel_size]))
        struct.pack_into("<3d", data, 608, *(3 * [voxel_size]))
        struct.pack_into("<II", data, 632, 2, 1)
        struct.pack_into("<Q", data, 696, 64)
        struct.pack_into("<I", data, 724, 1)
        struct.pack_into("<Q", data, 728, 4096**3)
        struct.pack_into("<3i", data, 748, 4095, 4095, 4095)
        struct.pack_into("<I", data, 760, 1)
        struct.pack_into("<4f", data, 768, value, value, value, 0)
        struct.pack_into("<If", data, 816, 1, value)
        return data

    def render(self, data):
        width = 4
        compute = self.get_compute()
        self.assertTrue(self.compiler.compile_shader(str(self.shader)), self.compiler.get_diagnostics())
        pixels = np.zeros((width * width, 4), dtype=np.float32)
        with compute.array(np.frombuffer(data, dtype=np.uint64)) as data_in, \
                compute.array(np.frombuffer(self.material, dtype=np.uint32)) as constants, \
                compute.array(pixels) as data_out:
            self.assertTrue(compute.dispatch_shader_on_array(
                str(self.shader), (width, width, 1), data_in.raw, constants.raw, data_out.raw))
            with compute.mapped_array(data_out.raw, pixels.dtype) as mapped:
                return mapped.copy().reshape(pixels.shape)

    def device_name(self):
        device_interface = self.get_compute().device_interface()
        native = device_interface.get_device_interface().contents
        desc = PhysicalDeviceDesc()
        device_index = native.get_device_index(device_interface.get_device())
        self.assertTrue(native.enumerate_devices(
            device_interface._device_manager, device_index,
            ctypes.cast(ctypes.byref(desc), ctypes.POINTER(ctypes.c_void_p))))
        return desc.device_name.decode("utf-8")

    def test_smoke_pixels_include_shadow_and_transmittance(self):
        grid = self.constant_grid(0.5)
        lit = self.render(grid)
        self.material.shadow_factor = 0
        unshadowed = self.render(grid)
        self.assertTrue(np.isfinite(lit).all())
        self.assertTrue((lit[:, :3] > 0).all())
        self.assertTrue((lit[:, :3] < unshadowed[:, :3]).all())
        np.testing.assert_array_equal(lit[:, 3], unshadowed[:, 3])
        alpha = float(np.float16(0.904902)) * 0.5 * (1 - np.exp(-0.05 / 16))
        np.testing.assert_allclose(lit[:, 3], (1 - alpha)**64, rtol=2e-5)

    def test_temperature_grid_changes_rendered_color(self):
        self.material.point_count = 2
        self.material.point_positions0[:] = [0, 1, 1, 1]
        self.material.point_color0[:] = [0.1, 0.1, 0.1, 0.5]
        self.material.point_color1[:] = [3, 1, 0.1, 0.5]
        smoke = self.constant_grid(0.5)
        cold = self.render(smoke + self.constant_grid(0.1))
        hot = self.render(smoke + self.constant_grid(0.9))
        self.assertTrue((hot[:, 0] > 2 * cold[:, 0]).all())
        np.testing.assert_allclose(hot[:, 3], cold[:, 3], atol=1e-6)

    def test_shadow_budget_keeps_full_volume(self):
        self.material.step_size_scale = 4096 / 33
        self.material.point_count = 1
        self.material.point_color0[:] = [0.9, 0.9, 0.9, 0.904902]
        self.material.shadow_num_steps = 128
        grid = self.constant_grid(0.5)
        lit = self.render(grid)
        self.assertTrue(np.isfinite(lit).all())
        self.assertTrue((lit[:, :3] > 0).all())
        alpha = float(np.float16(0.904902)) * 0.5 * (1 - np.exp(-0.05 * 4 / 33))
        np.testing.assert_allclose(lit[:, 3], (1 - alpha)**33, rtol=2e-5)

    def test_maximum_ray_steps_keep_full_volume(self):
        self.material.step_size_scale = 0.01
        self.material.shadow_num_steps = 128
        lit = self.render(self.constant_grid(0.5))
        self.assertTrue(np.isfinite(lit).all())
        self.assertTrue((lit[:, :3] > 0).all())
        alpha = float(np.float16(0.904902)) * 0.5 * (1 - np.exp(-0.05 / 1024))
        expected = (1 - alpha)**4096
        try:
            np.testing.assert_allclose(lit[:, 3], expected, rtol=4e-4)
        except AssertionError:
            name = self.device_name()
            if name.lower().startswith(("llvmpipe", "lavapipe")) and (lit[:, 3] > expected).all():
                pytest.xfail(f"{name} truncates nested ray loops; see docs/flow-smoke.md")
            raise


if __name__ == "__main__":
    unittest.main()
