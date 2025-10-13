# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
#
from nanovdb_editor import Compiler, Compute, pnanovdb_CompileTarget, MemoryBuffer
from ctypes import *

import os
import sys
import numpy as np
import unittest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_SHADER = os.path.join(SCRIPT_DIR, "shaders/test.slang")


class TestDispatch(unittest.TestCase):
    def setUp(self):
        self.element_count = 8
        self.array_dtype = np.dtype(np.int32)
        self.input_data = np.array([i for i in range(self.element_count)], dtype=self.array_dtype)
        self.constants_data = np.array([4], dtype=self.array_dtype)
        self.output_data = np.zeros_like(self.input_data)

        self.compiler = Compiler()
        self.compiler.create_instance()

        self.compute = Compute(self.compiler)
        self.compute.device_interface().create_device_manager()
        self.compute.device_interface().create_device()

    def assert_addition_result(self, result):
        for i, val in enumerate(self.input_data):
            self.assertEqual(result[i], val + self.constants_data[0])

    def test_vulkan_dispatch(self):
        self.compiler.compile_shader(TEST_SHADER, entry_point_name="computeMain")

        input_array = self.compute.create_array(self.input_data)
        constants_array = self.compute.create_array(self.constants_data)
        output_array = self.compute.create_array(self.output_data)

        success = self.compute.dispatch_shader_on_array(
            TEST_SHADER, (1, 1, 1), input_array, constants_array, output_array
        )
        self.assertTrue(success)

        result = self.compute.map_array(output_array, self.array_dtype)
        self.assertIsNotNone(result)
        self.assert_addition_result(result)

        self.compute.unmap_array(output_array)

        self.compute.destroy_array(input_array)
        self.compute.destroy_array(constants_array)
        self.compute.destroy_array(output_array)

    def test_cpu_dispatch(self):
        self.compiler.compile_shader(
            TEST_SHADER, entry_point_name="computeMain", compile_target=pnanovdb_CompileTarget.CPU
        )

        class Constants(Structure):
            """Definition equivalent to constants_t in the shader."""

            _fields_ = [
                ("magic_number", c_int32),
            ]

        constants = Constants()
        constants.magic_number = self.constants_data[0]

        class UniformState(Structure):
            _fields_ = [
                ("data_in", MemoryBuffer),
                ("constants", c_void_p),  # Constant buffer must be passed as a pointer
                ("data_out", MemoryBuffer),
            ]

        uniform_state = UniformState()
        uniform_state.data_in = MemoryBuffer(self.input_data)
        uniform_state.constants = c_void_p(addressof(constants))
        uniform_state.data_out = MemoryBuffer(self.output_data)

        success = self.compiler.execute_cpu(TEST_SHADER, (1, 1, 1), None, c_void_p(addressof(uniform_state)))
        self.assertTrue(success)

        data_out = uniform_state.data_out.to_ndarray(self.array_dtype)
        self.assert_addition_result(data_out)

        # Prevent destructor from calling into native lib during shutdown
        self.compiler._instance = None
        self.compiler._compiler = None


if __name__ == "__main__":
    unittest.main()
