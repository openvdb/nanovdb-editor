#!/usr/bin/env python3
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import pathlib
import time

import nanovdb_editor as nve


def main() -> None:
    shader_path = pathlib.Path("pytests/shaders/test.slang").resolve()
    if not shader_path.is_file():
        raise SystemExit(f"Expected shader test file at {shader_path}")

    compiler = nve.Compiler()
    compiler.create_instance()
    print("Compiler instance created")

    compute = nve.Compute(compiler)
    print("Compute interface loaded")
    device_interface = compute.device_interface()
    device_interface.create_device_manager()
    device_interface.create_device()
    print("Compute device created")

    compiler.clear_diagnostics()
    if not compiler.compile_shader(
        str(shader_path), entry_point_name="computeMain"
    ):
        raise AssertionError(
            "Vulkan shader smoke compile failed.\n"
            f"Compiler diagnostics:\n{compiler.get_diagnostics() or '<none>'}"
        )
    print("Vulkan shader compile smoke test passed")

    if nve.has_slang_llvm_runtime():
        compiler.clear_diagnostics()
        if not compiler.compile_shader(
            str(shader_path),
            entry_point_name="computeMain",
            compile_target=nve.CompileTarget.CPU,
        ):
            raise AssertionError(
                "CPU shader smoke compile failed.\n"
                "Compiler diagnostics:\n"
                f"{compiler.get_diagnostics() or '<none>'}"
            )
        print("CPU shader compile smoke test passed")
    else:
        print(
            "Skipping CPU shader smoke test because slang-llvm is unavailable"
        )

    editor = nve.Editor(compute, compiler)
    config = nve.EditorConfig()
    config.ip_address = b"127.0.0.1"
    config.port = 8080
    config.headless = 1
    config.streaming = 0
    print("Starting editor smoke test")
    editor.start(config)
    time.sleep(0.5)
    editor.stop()
    print("Editor start/stop smoke test passed")

    editor = None
    compute = None
    compiler._instance = None
    compiler._compiler = None


if __name__ == "__main__":
    main()
