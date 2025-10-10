# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from nanovdb_editor import (
    Compiler,
    Compute,
    CompileTarget,
    MemoryBuffer,
)
from ctypes import Structure, c_float, c_void_p, addressof

import os
import gc
import numpy as np
import unittest
from parameterized import parameterized


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_SHADER = os.path.join(SCRIPT_DIR, "shaders/test_matrix.slang")
TEST_SHADER_IN = os.path.join(SCRIPT_DIR, "shaders/test_matrix_in.slang")

# Test data
MATRIX_SIZE = 4
constants_data = np.arange(MATRIX_SIZE * MATRIX_SIZE, dtype=np.float32)
array_dtype_out = np.dtype(np.int32)

# TEST setup
# test_matrix.slang - shader creates the matrix in row-major order and stores
# it in the output buffer in row-major order
# test_matrix_in.slang - shader reads the matrix from the row-major initialized
# constants buffer and stores it in the output buffer in row-major order

# TEST results
# test_matrix.slang - the output buffer is filled with the matrix in row-major
# order regardless of row major setting
# test_matrix_in.slang - when row major is False, the matrix is stored in
# column-major order in the output buffer


TEST_CASES = [
    ("out_vk_col", TEST_SHADER, CompileTarget.VULKAN, False),
    ("out_vk_row", TEST_SHADER, CompileTarget.VULKAN, True),
    ("out_cpu_col", TEST_SHADER, CompileTarget.CPU, False),
    ("out_cpu_row", TEST_SHADER, CompileTarget.CPU, True),
    ("in_vk_col", TEST_SHADER_IN, CompileTarget.VULKAN, False),
    ("in_vk_row", TEST_SHADER_IN, CompileTarget.VULKAN, True),
    ("in_cpu_col", TEST_SHADER_IN, CompileTarget.CPU, False),
    ("in_cpu_row", TEST_SHADER_IN, CompileTarget.CPU, True),
]


class TestMatrix(unittest.TestCase):

    def setUp(self):
        self.compiler = Compiler()
        self.compute = Compute(self.compiler)

        self.compiler.create_instance()
        self.compute.device_interface().create_device_manager()
        self.compute.device_interface().create_device()

    def tearDown(self):
        self.compute = None
        self.compiler = None
        gc.collect()

    def _assert_matrix_result(self, test_shader, is_row_major, result):
        for idx, val in enumerate(constants_data):
            if test_shader == TEST_SHADER_IN and not is_row_major:
                row = idx // MATRIX_SIZE
                col = idx % MATRIX_SIZE
                idx_mapped = col * MATRIX_SIZE + row
            else:
                idx_mapped = idx
            # result is int32, constants are float32; numeric equality expected
            self.assertEqual(result[idx_mapped], val)

    @parameterized.expand(TEST_CASES)
    def test_matrix(self, _case_name, test_shader, target, is_row_major):
        if target == CompileTarget.VULKAN:
            input_data = np.zeros(len(constants_data), dtype=array_dtype_out)
            output_data = np.zeros(len(constants_data), dtype=array_dtype_out)

            self.compiler.compile_shader(
                test_shader,
                entry_point_name="computeMain",
                is_row_major=is_row_major,
            )

            input_array = self.compute.create_array(input_data)
            constants_array = self.compute.create_array(constants_data)
            output_array = self.compute.create_array(output_data)

            try:
                success = self.compute.dispatch_shader_on_array(
                    test_shader,
                    (MATRIX_SIZE, MATRIX_SIZE, 1),
                    input_array,
                    constants_array,
                    output_array,
                )
                self.assertTrue(success)
                result = self.compute.map_array(output_array, array_dtype_out)
                self.assertIsNotNone(result)
                self._assert_matrix_result(test_shader, is_row_major, result)
            finally:
                self.compute.unmap_array(output_array)
                self.compute.destroy_array(input_array)
                self.compute.destroy_array(constants_array)
                self.compute.destroy_array(output_array)

        elif target == CompileTarget.CPU:
            input_data = np.zeros(len(constants_data), dtype=array_dtype_out)
            output_data = np.zeros(len(constants_data), dtype=array_dtype_out)

            self.compiler.compile_shader(
                test_shader,
                entry_point_name="computeMain",
                compile_target=CompileTarget.CPU,
                is_row_major=is_row_major,
            )

            class Constants(Structure):
                """Definition equivalent to constants_t in the shader."""

                _fields_ = [
                    ("matrix", c_float * 16),
                ]

            constants = Constants()
            constants.matrix = (c_float * 16)(*[float(x) for x in constants_data])

            class UniformState(Structure):
                _fields_ = [
                    ("data_in", MemoryBuffer),
                    # Constant buffer passed as pointer
                    ("constants", c_void_p),
                    ("data_out", MemoryBuffer),
                ]

            uniform_state = UniformState()
            uniform_state.data_in = MemoryBuffer(input_data)
            uniform_state.constants = c_void_p(addressof(constants))
            uniform_state.data_out = MemoryBuffer(output_data)

            success = self.compiler.execute_cpu(
                test_shader,
                (MATRIX_SIZE, MATRIX_SIZE, 1),
                None,
                c_void_p(addressof(uniform_state)),
            )
            self.assertTrue(success)
            data_out = uniform_state.data_out.to_ndarray(array_dtype_out)
            self._assert_matrix_result(test_shader, is_row_major, data_out)

            # Prevent destructor from calling into native lib during shutdown
            self.compiler._instance = None
            self.compiler._compiler = None


if __name__ == "__main__":
    unittest.main()
