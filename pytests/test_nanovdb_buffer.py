# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import struct

import numpy as np
import pytest

import nanovdb_editor as nve


def raw_empty_grid():
    data = bytearray(800)
    data[:8] = b"NanoVDB1"
    struct.pack_into("<I", data, 16, 32 << 21)
    struct.pack_into("<I", data, 28, 1)
    struct.pack_into("<Q", data, 32, len(data))
    struct.pack_into("<I", data, 636, 1)
    struct.pack_into("<q", data, 696, 64)
    return data


@pytest.fixture
def app():
    with nve.create_default(device=False) as session:
        yield session


@pytest.mark.parametrize("wrap", [bytes, bytearray, memoryview, lambda data: np.frombuffer(data, dtype=np.uint32)])
def test_buffer_copy_survives_source_mutation(app, wrap):
    source = raw_empty_grid()
    supplied = wrap(source)
    expected = bytes(source)
    with app.scene("smoke").nanovdb_from_buffer(supplied, register=False) as grid:
        source[:] = b"\0" * len(source)
        del supplied
        assert grid.to_numpy().tobytes() == expected


def test_consecutive_grids_round_trip(app):
    source = raw_empty_grid() + raw_empty_grid()
    with app.scene("smoke").nanovdb_from_buffer(source, register=False) as grid:
        assert grid.to_numpy().tobytes() == source


@pytest.mark.parametrize("data, message", [
    (b"", "empty"),
    (b"NanoVDB1", "truncated"),
    (b"NanoVDB2" + bytes(792), "file container"),
    (np.arange(1000, dtype=np.uint8)[::2], "contiguous"),
    ([1, 2, 3], "buffer protocol"),
])
def test_reject_invalid_buffer_before_native_call(app, data, message):
    with pytest.raises(nve.InvalidArgumentError, match=message):
        app.scene("smoke").nanovdb_from_buffer(data)


@pytest.mark.parametrize("size", [0, 735, 799, 832, 2**63])
def test_reject_invalid_declared_size(app, size):
    data = raw_empty_grid()
    struct.pack_into("<Q", data, 32, size)
    with pytest.raises(nve.InvalidArgumentError, match="grid size"):
        app.scene("smoke").nanovdb_from_buffer(data)


def test_reject_trailing_partial_grid(app):
    with pytest.raises(nve.InvalidArgumentError, match="truncated"):
        app.scene("smoke").nanovdb_from_buffer(raw_empty_grid() + b"NanoVDB1")


def test_shader_requires_registration(app):
    with pytest.raises(nve.InvalidArgumentError, match="register"):
        app.scene("smoke").nanovdb_from_buffer(raw_empty_grid(), register=False, shader="editor/editor.slang")


def test_custom_shader_values_and_replacement(app):
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid(), shader="editor/editor.slang",
                                   shader_parameters={"alpha_scale": 0.25}):
        pass
    with scene.nanovdb_from_buffer(raw_empty_grid(), shader="editor/editor.slang",
                                   shader_parameters={"alpha_scale": 0.75}):
        pass
    scene.set_shader("nanovdb", "editor/editor.slang", parameters={"narrow_band_only": False})
    for params in ({"unknown_field": 1}, {"alpha_scale": "wrong"}, {"slice_plane": [1, 2]},
                   {"narrow_band_only": -1}):
        with pytest.raises(nve.PipelineError):
            scene.set_shader("nanovdb", "editor/editor.slang", parameters=params)
    with pytest.raises(nve.InvalidArgumentError, match="finite"):
        scene.set_shader("nanovdb", "editor/editor.slang", parameters={"alpha_scale": float("nan")})
    with pytest.raises(nve.PipelineError, match="does not exist"):
        scene.set_shader("missing", "editor/editor.slang")
    scene.remove("nanovdb")


def test_camera_update_before_start_preserves_native_defaults(app):
    scene = app.scene("camera_defaults")
    assert scene.get_camera() is None
    camera = scene.update_camera(position=(1, 2, 3), eye_distance=4)
    assert camera.config.fov_angle_y > 0
    assert camera.config.near_plane > 0
    assert camera.config.far_plane > camera.config.near_plane
    assert camera.state.orthographic_scale > 0
    assert camera.state.position.x == 1
    assert camera.state.position.y == 2
    assert camera.state.position.z == 3
    assert camera.state.eye_distance_from_position == 4


@pytest.mark.parametrize("existing", [False, True])
@pytest.mark.parametrize("failure", ["compile", "parameter", "json"])
def test_failed_material_update_keeps_registered_buffer(app, tmp_path, existing, failure):
    scene = app.scene("material_failure")
    shader = "editor/editor.slang"
    if existing:
        with scene.nanovdb_from_buffer(raw_empty_grid(), shader=shader,
                                       shader_parameters={"alpha_scale": 0.25}):
            pass
    parameters = None
    error = nve.PipelineError
    if failure == "compile":
        shader = tmp_path / "missing.slang"
    elif failure == "parameter":
        parameters = {"unknown_field": 1}
    else:
        parameters = {"alpha_scale": float("nan")}
        error = nve.InvalidArgumentError

    with pytest.raises(error):
        scene.nanovdb_from_buffer(raw_empty_grid(), shader=shader, shader_parameters=parameters)
    assert scene.get_render_pipeline("nanovdb").type_id == "nanovdb_render"


@pytest.mark.parametrize("parameters", [None, {}])
def test_parameterless_custom_shader_accepts_empty_parameters(app, tmp_path, parameters):
    shader = tmp_path / f"parameterless_{tmp_path.name}.slang"
    shader.write_text("""
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = float4(0, 1, 0, 0); }
""")
    scene = app.scene("parameterless")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", shader, parameters=parameters)
    with pytest.raises(nve.PipelineError, match="Unknown shader parameter"):
        scene.set_shader("nanovdb", shader, parameters={"unknown": 1})


@pytest.mark.parametrize("value", [0.5, 1, -2.0, 65504.0])
def test_half_shader_parameters_accept_numeric_values(app, tmp_path, value):
    shader = tmp_path / f"half_numeric_{tmp_path.name}.slang"
    shader.write_text("""
struct shader_params_t { half value; };
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = float4(float(shader_params.value), 0, 0, 0); }
""")
    scene = app.scene("half_numeric")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", shader, parameters={"value": value})
    with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
        scene.set_shader("nanovdb", shader, parameters={"value": 1e10})
