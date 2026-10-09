# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import ctypes
import os
from pathlib import Path
import subprocess
import sys

import nanovdb_editor as nve
import numpy as np
import pytest

from test_dispatch import cpu_target_supported


@pytest.fixture
def compiler():
    instance = nve.Compiler()
    instance.create_instance()
    try:
        yield instance
    finally:
        instance.destroy_instance()


@pytest.fixture
def external_shader(tmp_path):
    directory = tmp_path / "external shaders"
    directory.mkdir()
    shader = directory / f"external_{tmp_path.parent.name}_{tmp_path.name}.slang"
    shader.write_text("""
#include "external_value.slang"
RWStructuredBuffer<float> data_out;
[shader("compute")][numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) { data_out[id.x] = external_value(); }
""")
    include = directory / "external_value.slang"
    include.write_text("float external_value() { return 0.25; }\n")
    return shader, include


@pytest.mark.parametrize("relative", [False, True], ids=["absolute", "relative"])
def test_external_shader_includes_and_cache(compiler, external_shader, monkeypatch, relative):
    shader, include = external_shader
    monkeypatch.chdir(shader.parent.parent)
    source = os.path.relpath(shader) if relative else str(shader)
    assert compiler.compile_shader(source, entry_point_name="computeMain"), compiler.get_diagnostics()

    include.write_text("float renamed_value() { return 0.5; }\n")
    assert not compiler.compile_shader(source, entry_point_name="computeMain")
    assert "external_value" in compiler.get_diagnostics()

    include.write_text("float external_value() { return 0.5; }\n")
    assert compiler.compile_shader(source, entry_point_name="computeMain"), compiler.get_diagnostics()


def test_external_shader_keeps_working_directory_includes(compiler, external_shader, monkeypatch):
    shader, include = external_shader
    directory = shader.parent.parent
    include.rename(directory / include.name)
    monkeypatch.chdir(directory)
    assert compiler.compile_shader(str(shader), entry_point_name="computeMain"), compiler.get_diagnostics()


@pytest.mark.skipif(not cpu_target_supported(), reason="CPU shader target is unavailable on this architecture")
@pytest.mark.parametrize("relative", [False, True], ids=["absolute", "relative"])
def test_external_cpu_shader_keeps_source_directory_clean(compiler, external_shader, monkeypatch, relative):
    shader, _ = external_shader
    monkeypatch.chdir(shader.parent.parent)
    source = os.path.relpath(shader) if relative else str(shader)
    source_files = set(shader.parent.iterdir())
    original_cwd = Path.cwd()
    try:
        assert compiler.compile_shader(
            source, entry_point_name="computeMain", compile_target=nve.CompileTarget.CPU
        ), compiler.get_diagnostics()
        assert Path.cwd() == original_cwd
    finally:
        os.chdir(original_cwd)
    assert set(shader.parent.iterdir()) == source_files

    class UniformState(ctypes.Structure):
        _fields_ = [("data_out", nve.MemoryBuffer)]

    output = np.zeros(1, dtype=np.float32)
    uniforms = UniformState(nve.MemoryBuffer(output))
    assert compiler.execute_cpu(source, (1, 1, 1), None, ctypes.addressof(uniforms))
    assert output[0] == 0.25


@pytest.mark.skipif(os.name == "nt", reason="Windows cannot delete the current working directory")
def test_external_shader_with_deleted_working_directory(external_shader, tmp_path):
    shader, _ = external_shader
    shader.write_text("""
RWStructuredBuffer<float> data_out;
[shader("compute")][numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) { data_out[id.x] = 0.25; }
""")
    directory = tmp_path / "deleted working directory"
    directory.mkdir()
    result = subprocess.run(
        [sys.executable, "-c", """
import os
import sys
import nanovdb_editor as nve

compiler = nve.Compiler()
compiler.create_instance()
os.rmdir(os.getcwd())
try:
    assert compiler.compile_shader(sys.argv[1], entry_point_name="computeMain"), compiler.get_diagnostics()
finally:
    os.chdir(os.path.dirname(sys.argv[1]))
    compiler.destroy_instance()
""", str(shader)],
        cwd=directory, capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
