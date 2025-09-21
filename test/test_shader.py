# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from nanovdb_editor import Compiler, Compute, pnanovdb_CompileTarget, MemoryBuffer
from ctypes import *

import os
import numpy as np


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_SHADER = os.path.join(SCRIPT_DIR, "../test/shaders/test.slang")

if __name__ == "__main__":

    print("Running Vulkan shader test...")

    # Test data
    ELEMENT_COUNT = 8
    array_dtype = np.dtype(np.int32)
    input_data = np.array([i for i in range(ELEMENT_COUNT)], dtype=array_dtype)
    constants_data = np.array([4], dtype=array_dtype)

    compiler = Compiler()
    compiler.create_instance()

    # Test Vulkan target
    compiler.compile_shader(TEST_SHADER, entry_point_name="computeMain")

    output_data = np.zeros_like(input_data)

    compute = Compute(compiler)
    input_array = compute.create_array(input_data)
    constants_array = compute.create_array(constants_data)
    output_array = compute.create_array(output_data)

    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    success = compute.dispatch_shader_on_array(
        TEST_SHADER,
        (1, 1, 1),
        input_array,
        constants_array,
        output_array
    )
    if success:
        result = compute.map_array(output_array, array_dtype)
        print(result)

        for i, val in enumerate(input_data):
            if result[i] != val + constants_data[0]:
                print("Error: Vulkan shader test failed!")
                break
        else:
            print("Vulkan shader test was successful")
    else:
        print("Error: Failed to dispatch Vulkan shader")

    compute.unmap_array(output_array)

    compute.destroy_array(input_array)
    compute.destroy_array(constants_array)
    compute.destroy_array(output_array)
