# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from nanovdb_editor import Compiler, Compute, pnanovdb_CompileTarget, MemoryBuffer
from ctypes import *

import os
import numpy as np


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TEST_SHADER = os.path.join(SCRIPT_DIR, "../test/shaders/test.slang")

if __name__ == "__main__":

    print(f"Current Process ID (PID): {os.getpid()}")

    # Test data
    ELEMENT_COUNT = 8
    array_dtype = np.dtype(np.int32)
    input_data = np.array([i for i in range(ELEMENT_COUNT)], dtype=array_dtype)
    constants_data = np.array([4], dtype=array_dtype)

    compiler = Compiler()
    compiler.create_instance()

    # Test CPU target
    compiler.compile_shader(
        TEST_SHADER,
        entry_point_name="computeMain",
        compile_target=pnanovdb_CompileTarget.CPU
    )

    class Constants(Structure):
        """Definition equivalent to constants_t in the shader."""
        _fields_ = [
            ("magic_number", c_int32),
        ]

    constants = Constants()
    constants.magic_number = constants_data[0]

    output_data = np.zeros_like(input_data)

    class UniformState(Structure):
        _fields_ = [
            ("data_in", MemoryBuffer),
            ("constants", c_void_p),        # Constant buffer must be passed as a pointer
            ("data_out", MemoryBuffer),
        ]

    uniform_state = UniformState()
    uniform_state.data_in = MemoryBuffer(input_data)
    uniform_state.constants = c_void_p(addressof(constants))
    uniform_state.data_out = MemoryBuffer(output_data)

    success = compiler.execute_cpu(
        TEST_SHADER,
        (1, 1, 1),
        None,
        c_void_p(addressof(uniform_state))
    )
    if success:
        data_out = uniform_state.data_out.to_ndarray(array_dtype)
        print(data_out)
        for i, val in enumerate(input_data):
            if data_out[i] != val + constants_data[0]:
                print("Error: CPU shader test failed!")
                break
        else:
            print("CPU shader test was successful")
    else:
        print("CPU shader test failed!")
