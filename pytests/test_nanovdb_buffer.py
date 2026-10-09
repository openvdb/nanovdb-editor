# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import ctypes
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
import importlib
import json
import os
from pathlib import Path
import struct
from threading import Event

import numpy as np
import pytest

import nanovdb_editor as nve

from test_dispatch import cpu_target_supported


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


def shader_state(app, scene="smoke", name="nanovdb"):
    from nanovdb_editor._shader import Shader, SHADER_TYPE

    with app.editor.params(app.editor.get_token(scene), app.editor.get_token(name),
                           ctypes.byref(SHADER_TYPE)) as address:
        assert address
        mapped = ctypes.cast(address, ctypes.POINTER(Shader)).contents
        return mapped.shader_name.contents.str, bytes(mapped.shader_params)


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
    assert struct.unpack_from("=f", shader_state(app)[1])[0] == 0.25
    with scene.nanovdb_from_buffer(raw_empty_grid(), shader="editor/editor.slang",
                                   shader_parameters={"alpha_scale": 0.75}):
        pass
    assert struct.unpack_from("=f", shader_state(app)[1])[0] == 0.75
    scene.set_shader("nanovdb", "editor/editor.slang", parameters={"narrow_band_only": False})
    for params in ({"unknown_field": 1}, {"alpha_scale": "wrong"}, {"slice_plane": [1, 2]},
                   {"narrow_band_only": -1}, {"narrow_band_only": 1.0}, {"alpha_scale": True}):
        with pytest.raises(nve.PipelineError):
            scene.set_shader("nanovdb", "editor/editor.slang", parameters=params)
    with pytest.raises(nve.InvalidArgumentError, match="finite"):
        scene.set_shader("nanovdb", "editor/editor.slang", parameters={"alpha_scale": float("nan")})
    with pytest.raises(nve.PipelineError, match="does not exist"):
        scene.set_shader("missing", "editor/editor.slang")
    scene.remove("nanovdb")


def test_shader_mapping_preserves_defaults_and_rejects_changes_atomically(app, tmp_path):
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    default_name, defaults = shader_state(app)
    scene.set_shader("nanovdb", "editor/editor.slang", parameters={"alpha_scale": 0.25})
    expected = bytearray(defaults)
    struct.pack_into("=f", expected, 0, 0.25)
    assert shader_state(app) == (default_name, bytes(expected))

    for shader, parameters in [
        ("editor/wireframe.slang", {"unknown": 1}),
        ("editor/editor.slang", {"alpha_scale": 0.75, "narrow_band_only": -1}),
        ("editor/editor.slang", {"slice_plane": [1, 2]}),
        (tmp_path / "missing.slang", {}),
    ]:
        with pytest.raises(nve.PipelineError):
            scene.set_shader("nanovdb", shader, parameters=parameters)
        assert shader_state(app) == (default_name, bytes(expected))

    scene.set_shader("nanovdb", "editor/editor.slang")
    assert shader_state(app) == (default_name, defaults)


def test_concurrent_shader_assignments_preserve_matching_parameters(app, monkeypatch):
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    for shader in ("editor/editor.slang", "editor/wireframe.slang"):
        assert app.editor._compiler.compile_shader(shader)

    editor_module = importlib.import_module("nanovdb_editor.editor")
    original_memmove = editor_module.memmove
    first_copied = Event()
    second_finished = Event()

    def paused_memmove(destination, source, count):
        result = original_memmove(destination, source, count)
        if not first_copied.is_set():
            first_copied.set()
            assert second_finished.wait(10), "Second shader assignment did not finish"
        return result

    monkeypatch.setattr(editor_module, "memmove", paused_memmove)
    with ThreadPoolExecutor(max_workers=1) as executor:
        first = executor.submit(scene.set_shader, "nanovdb", "editor/editor.slang", parameters={"alpha_scale": 0.25})
        try:
            assert first_copied.wait(10), "First shader assignment did not copy its parameters"
            scene.set_shader("nanovdb", "editor/wireframe.slang", parameters={"highlight_bbox": 1})
        finally:
            second_finished.set()
        first.result(timeout=10)

    shader_name, parameters = shader_state(app)
    assert shader_name == b"editor/editor.slang"
    assert struct.unpack_from("=f", parameters)[0] == 0.25


@pytest.mark.parametrize("after_copy", [False, True])
def test_interrupted_shader_assignment_preserves_complete_state(app, monkeypatch, after_copy):
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", "editor/wireframe.slang", parameters={"highlight_bbox": 1})
    new_state = shader_state(app)
    scene.set_shader("nanovdb", "editor/editor.slang", parameters={"alpha_scale": 0.25})
    old_state = shader_state(app)

    editor_module = importlib.import_module("nanovdb_editor.editor")
    original_memmove = editor_module.memmove
    original_map = app.editor.map_params
    original_unmap = app.editor.unmap_params
    mapping_calls = []

    def map_params(*args):
        address = original_map(*args)
        assert address
        mapping_calls.append("map")
        return address

    def unmap_params(*args):
        mapping_calls.append("unmap")
        return original_unmap(*args)

    def interrupted_memmove(*args):
        if after_copy:
            original_memmove(*args)
        raise KeyboardInterrupt("shader publication interrupted")

    with monkeypatch.context() as patch:
        patch.setattr(editor_module, "memmove", interrupted_memmove)
        patch.setattr(app.editor, "map_params", map_params)
        patch.setattr(app.editor, "unmap_params", unmap_params)
        with pytest.raises(KeyboardInterrupt, match="shader publication interrupted"):
            scene.set_shader("nanovdb", "editor/wireframe.slang", parameters={"highlight_bbox": 1})

    assert mapping_calls == ["map", "unmap"]
    assert shader_state(app) == (new_state if after_copy else old_state)


@pytest.mark.parametrize("body_error", [False, True])
def test_failed_nested_params_mapping_preserves_outer_shader(app, monkeypatch, body_error):
    from nanovdb_editor._shader import Shader, SHADER_TYPE

    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_custom_params({"SceneParams": {"Frame": {"type": "uint", "value": 0}}})
    scene_token = app.editor.get_token("smoke")
    name_token = app.editor.get_token("nanovdb")
    empty_scene = app.editor.get_token("empty")
    custom_type = app.editor.get_custom_scene_params_data_type(scene_token)
    before = shader_state(app)

    with ThreadPoolExecutor(max_workers=1) as executor:
        with app.editor.params(scene_token, name_token, ctypes.byref(SHADER_TYPE)) as address:
            assert address
            mapped = ctypes.cast(address, ctypes.POINTER(Shader)).contents
            struct.pack_into("=f", mapped.shader_params, 0, 0.5)
            unmap_calls = []
            with monkeypatch.context() as patch:
                patch.setattr(app.editor, "unmap_params", lambda *args: unmap_calls.append(args))
                expected_error = pytest.raises(RuntimeError, match="inner mapping failed") if body_error else nullcontext()
                with expected_error:
                    with app.editor.params(empty_scene, None, custom_type) as missing:
                        assert missing is None
                        if body_error:
                            raise RuntimeError("inner mapping failed")
            assert not unmap_calls
            assert struct.unpack_from("=f", mapped.shader_params)[0] == 0.5
            assert executor.submit(shader_state, app).result(timeout=10) == before
            struct.pack_into("=f", mapped.shader_params, 0, 0.75)

    expected = bytearray(before[1])
    struct.pack_into("=f", expected, 0, 0.75)
    assert shader_state(app) == (before[0], bytes(expected))


@pytest.mark.parametrize("mapping", ["pipeline", "process_step"])
@pytest.mark.parametrize("body_error", [False, True])
def test_failed_nested_pipeline_mapping_unmaps_only_outer(app, monkeypatch, mapping, body_error):
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_pipeline("nanovdb", nve.PipelineStage.PROCESS, "voxelbvh")
    scene_token = app.editor.get_token("smoke")
    name_token = app.editor.get_token("nanovdb")
    index = nve.PipelineStage.PROCESS if mapping == "pipeline" else 0
    outer_args = (scene_token, name_token, index)
    if mapping == "pipeline":
        inner_args = (scene_token, app.editor.get_token("missing"), index)
    else:
        inner_args = (scene_token, name_token, app.editor.get_process_step_count(scene_token, name_token))
    context = getattr(app.editor, mapping + "_params")
    unmap_name = "unmap_" + mapping + "_params"
    original_unmap = getattr(app.editor, unmap_name)
    unmap_calls = []

    def unmap_params(*args):
        unmap_calls.append(args)
        if args == outer_args:
            original_unmap(*args)

    monkeypatch.setattr(app.editor, unmap_name, unmap_params)
    expected_error = pytest.raises(RuntimeError, match="inner mapping failed") if body_error else nullcontext()
    with expected_error:
        with context(*outer_args) as outer:
            assert outer is not None
            with context(*inner_args) as missing:
                assert missing is None
                if body_error:
                    raise RuntimeError("inner mapping failed")
            assert not unmap_calls
    assert unmap_calls == [outer_args]


def test_shader_update_inside_custom_params_mapping(app):
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_custom_params({"SceneParams": {"Frame": {"type": "uint", "value": 0}}})
    with scene.custom_params() as params:
        scene.set_shader("nanovdb", "editor/editor.slang", parameters={"alpha_scale": 0.5})
        params["Frame"] = 1
    assert struct.unpack_from("=f", shader_state(app)[1])[0] == 0.5
    with scene.custom_params() as params:
        assert params["Frame"] == 1


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


@pytest.mark.parametrize("value", [0.5, 1, -2.0, 65504.0, 1.00048828225])
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
    expected = struct.pack("=e", struct.unpack("=f", struct.pack("=f", value))[0])
    assert shader_state(app, "half_numeric")[1][:2] == expected
    with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
        scene.set_shader("nanovdb", shader, parameters={"value": 1e10})
    assert shader_state(app, "half_numeric")[1][:2] == expected


def test_half_shader_vector_values_and_defaults(app, tmp_path):
    shader = tmp_path / f"half_vector_{tmp_path.name}.slang"
    shader.write_text("""
struct shader_params_t { half4 value; };
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = float4(shader_params.value); }
""")
    shader.with_suffix(".slang.json").write_text(
        '{"ShaderParams":{"value":{"value":[0.5,1,-2,65504]}}}')
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", shader)
    assert struct.unpack_from("=4e", shader_state(app)[1]) == (0.5, 1, -2, 65504)
    scene.set_shader("nanovdb", shader, parameters={"value": [-0.25, 0, 2, 3]})
    before = shader_state(app)
    assert struct.unpack_from("=4e", before[1]) == (-0.25, 0, 2, 3)
    with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
        scene.set_shader("nanovdb", shader, parameters={"value": [0, 1, 2, 1e10]})
    assert shader_state(app) == before


def test_boolean_shader_validation_and_default_values(app, tmp_path):
    shader = tmp_path / f"boolean_{tmp_path.name}.slang"
    shader.write_text("""
struct shader_params_t { bool enabled; int count; float value; };
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    texture_out[id.xy] = float4(shader_params.enabled, shader_params.count, shader_params.value, 1);
}
""")
    shader.with_suffix(".slang.json").write_text(
        '{"ShaderParams":{"enabled":{"value":true},"count":{"value":1.5},"value":{"value":null}}}')
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", shader)
    before = shader_state(app)
    assert struct.unpack_from("=Iif", before[1]) == (1, 1, 0)
    for parameters in [{"enabled": 2}, {"enabled": 1.0}, {"count": 1.0}, {"count": True}]:
        with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
            scene.set_shader("nanovdb", shader, parameters=parameters)
        assert shader_state(app) == before


def shader_with_hint(tmp_path, field_type, hint):
    shader = tmp_path / f"hint_{tmp_path.name}.slang"
    value = "shader_params.value[0]" if field_type.endswith(("2", "3", "4")) else "shader_params.value"
    shader.write_text(f"""
struct shader_params_t {{ {field_type} value; }};
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {{ texture_out[id.xy] = float4(float({value}), 0, 0, 0); }}
""")
    shader.with_suffix(".slang.json").write_text(json.dumps({"ShaderParams": {"value": hint}}))
    return shader


def native_shader_defaults(app, shader):
    from nanovdb_editor._shader import _struct_type, _TOKEN
    from nanovdb_editor.editor import EditorToken

    class ShaderName(ctypes.Structure):
        _fields_ = [("shader_name", ctypes.POINTER(EditorToken))]

    name_type = _struct_type(ShaderName, "pnanovdb_editor_shader_name_t", [("shader_name", _TOKEN, 1)])
    assert app.editor._compiler.compile_shader(str(shader))
    with app.editor.params(app.editor.get_token("smoke"), app.editor.get_token("nanovdb"),
                           ctypes.byref(name_type)) as address:
        assert address
        ctypes.cast(address, ctypes.POINTER(ShaderName)).contents.shader_name = app.editor.get_token(str(shader))
    return shader_state(app)


def test_mixed_shader_parameters_match_native_packing(app, tmp_path):
    source = """
struct shader_params_t {
    half2 small;
    float3 direction;
    uint _pad;
    double weight;
    int64_t count;
    uint2 indices;
    bool enabled;
    float tail;
};
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float value = float(shader_params.small.x) + shader_params.direction.y
        + float(shader_params.weight) + float(shader_params.count)
        + float(shader_params.indices.x) + float(shader_params.enabled)
        + shader_params.tail;
    texture_out[id.xy] = float4(value, 0, 0, 1);
}
"""
    defaults = {
        "small": [0.5, -2.0],
        "direction": [0.25, 1.0, -3.0],
        "_pad": 2**32 - 1,
        "weight": 0.625,
        "count": -(2**40),
        "indices": [3, 2**31],
        "enabled": True,
        "tail": -0.125,
    }
    overrides = {
        "small": [-0.25, 4.0],
        "direction": [-2.0, 0.5, 8.0],
        "count": 2**40 + 1,
        "enabled": False,
        "tail": 0.875,
    }
    shaders = []
    for suffix, values in [("defaults", defaults), ("overrides", {**defaults, **overrides})]:
        shader = tmp_path / f"mixed_{tmp_path.name}_{suffix}.slang"
        shader.write_text(source)
        hints = {name: {"value": value} for name, value in reversed(list(values.items()))}
        shader.with_suffix(".slang.json").write_text(json.dumps({"ShaderParams": hints}))
        shaders.append(shader)

    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    native = native_shader_defaults(app, shaders[0])
    scene.set_shader("nanovdb", shaders[0])
    assert shader_state(app) == native

    expected = native_shader_defaults(app, shaders[1])[1]
    assert expected != native[1]
    scene.set_shader("nanovdb", shaders[0], parameters=overrides)
    assert shader_state(app) == (native[0], expected)


@pytest.mark.parametrize("field_type, hint, format_code, expected", [
    ("float", {"value": [0.625]}, "f", (0.625,)),
    ("float", {"value": []}, "f", (0,)),
    ("float", {"value": [0.625, 0.75]}, "f", (0.625,)),
    ("float", None, "f", (0,)),
    ("float", [], "f", (0,)),
    ("float", "ignored", "f", (0,)),
    ("float", 1, "f", (0,)),
    ("float", {"value": "ignored"}, "f", (0,)),
    ("float", {"value": {}}, "f", (0,)),
    ("float", {"value": True}, "f", (0,)),
    ("float", {"value": [True]}, "f", (1,)),
    ("float3", {"value": [0.625]}, "3f", (0.625, 0, 0)),
    ("float3", {"value": 0.625}, "3f", (0.625, 0, 0)),
    ("float3", {"value": [1, 2, 3, 4]}, "3f", (1, 2, 3)),
    ("int", {"value": [1.5]}, "i", (1,)),
    ("int", {"value": [True]}, "i", (1,)),
    ("double", {"value": [0.625]}, "d", (0.625,)),
    ("half", {"value": [0.625]}, "e", (0.625,)),
    ("half", {"value": []}, "e", (0.0,)),
    ("half3", {"value": [0.625]}, "3e", (0.625, 0.0, 0.0)),
    ("bool", {"value": True}, "I", (1,)),
    ("bool", {"value": 1}, "I", (0,)),
    ("bool", {"value": [True]}, "I", (0,)),
])
def test_shader_hint_defaults_match_native_mapping(app, tmp_path, field_type, hint, format_code, expected):
    shader = shader_with_hint(tmp_path, field_type, hint)
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    native = native_shader_defaults(app, shader)
    assert struct.unpack_from("=" + format_code, native[1]) == expected
    scene.set_shader("nanovdb", shader)
    assert shader_state(app) == native
    with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
        scene.set_shader("nanovdb", shader, parameters={"value": []})
    assert shader_state(app) == native


@pytest.mark.parametrize("field_type, default, format_code, expected", [
    ("int", 2**31, "i", (-2**31,)),
    ("int", -2**31 - 1, "i", (2**31 - 1,)),
    ("int", 2**64 - 1, "i", (-1,)),
    ("uint", -1, "I", (2**32 - 1,)),
    ("uint", 2**32, "I", (0,)),
    ("uint", 2**64 - 1, "I", (2**32 - 1,)),
    ("int64_t", 2**63, "q", (-2**63,)),
    ("int64_t", 2**64 - 1, "q", (-1,)),
    ("uint64_t", -1, "Q", (2**64 - 1,)),
    ("uint64_t", -2**63, "Q", (2**63,)),
    ("int2", [2**31, -2**31 - 1], "2i", (-2**31, 2**31 - 1)),
    ("uint2", [-1, 2**32], "2I", (2**32 - 1, 0)),
])
def test_shader_integer_defaults_wrap_to_native_width(app, tmp_path, field_type, default, format_code, expected):
    shader = shader_with_hint(tmp_path, field_type, {"value": default})
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    native = native_shader_defaults(app, shader)
    assert struct.unpack_from("=" + format_code, native[1]) == expected
    scene.set_shader("nanovdb", shader)
    assert shader_state(app) == native
    with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
        scene.set_shader("nanovdb", shader, parameters={"value": default})
    assert shader_state(app) == native


@pytest.mark.parametrize("field_type, is_bool, accepts_boolean, format_code", [
    ("int", True, True, "i"),
    ("uint", True, True, "I"),
    ("int64_t", True, True, "q"),
    ("uint64_t", True, True, "Q"),
    ("double", True, True, "d"),
    ("int", "false", False, "i"),
    ("int", 1, False, "i"),
    ("int", [True], False, "i"),
    ("int", False, False, "i"),
    ("float", True, False, "f"),
    ("half", True, False, "e"),
    ("int2", True, False, "2i"),
    ("double2", True, False, "2d"),
    ("bool", False, True, "I"),
])
def test_shader_boolean_hint_override_validation(app, tmp_path, field_type, is_bool, accepts_boolean, format_code):
    shader = shader_with_hint(tmp_path, field_type, {"isBool": is_bool})
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", shader)
    before = shader_state(app)
    for value in (True, False):
        override = [value, value] if field_type.endswith("2") else value
        if accepts_boolean:
            scene.set_shader("nanovdb", shader, parameters={"value": override})
            assert struct.unpack_from("=" + format_code, shader_state(app)[1]) == (int(value),)
        else:
            with pytest.raises(nve.PipelineError, match="Invalid shader parameter"):
                scene.set_shader("nanovdb", shader, parameters={"value": override})
            assert shader_state(app) == before


def test_external_shader_uses_sibling_includes_and_defaults(app, tmp_path):
    shader = tmp_path / f"external_{tmp_path.parent.name}_{tmp_path.name}.slang"
    shader.write_text("""
#include "editor_params.slang"
#include "external_values.slang"
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = shader_params.value.x; }
""")
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    for field_type, defaults, overrides in (("float", [0.25], [0.75]), ("float2", [0.25, 0.5], [0.75, 1.0])):
        (tmp_path / "external_values.slang").write_text(f"struct shader_params_t {{ {field_type} value; }};\n")
        shader.with_suffix(".slang.json").write_text(json.dumps({"ShaderParams": {"value": {"value": defaults}}}))
        scene.set_shader("nanovdb", shader)
        name, values = shader_state(app)
        assert name == str(shader).encode()
        assert struct.unpack_from("=" + "f" * len(defaults), values) == tuple(defaults)
        scene.set_shader("nanovdb", shader, parameters={"value": overrides[0] if len(overrides) == 1 else overrides})
        assert struct.unpack_from("=" + "f" * len(overrides), shader_state(app)[1]) == tuple(overrides)


@pytest.mark.skipif(not cpu_target_supported(), reason="CPU shader target is unavailable on this architecture")
def test_external_cpu_shader_keeps_intermediates_out_of_source(app, tmp_path):
    shader = tmp_path / f"external_cpu_{tmp_path.parent.name}_{tmp_path.name}.slang"
    shader.write_text("""
#include "external_value.slang"
RWStructuredBuffer<float> data_out;
[shader("compute")][numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) { data_out[id.x] = external_value(); }
""")
    (tmp_path / "external_value.slang").write_text("float external_value() { return 0.25; }\n")
    source_files = set(tmp_path.iterdir())
    original_cwd = Path.cwd()
    compiler = app.editor._compiler
    try:
        assert compiler.compile_shader(str(shader), entry_point_name="computeMain",
                                       compile_target=nve.CompileTarget.CPU), compiler.get_diagnostics()
        assert Path.cwd() == original_cwd
    finally:
        os.chdir(original_cwd)
    assert set(tmp_path.iterdir()) == source_files

    class UniformState(ctypes.Structure):
        _fields_ = [("data_out", nve.MemoryBuffer)]

    output = np.zeros(1, dtype=np.float32)
    uniforms = UniformState(nve.MemoryBuffer(output))
    assert compiler.execute_cpu(str(shader), (1, 1, 1), None, ctypes.addressof(uniforms))
    assert output[0] == 0.25


def test_shader_symlink_uses_adjacent_defaults(app, tmp_path):
    source = tmp_path / "source.slang"
    source.write_text("""
struct shader_params_t { float value; };
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = shader_params.value; }
""")
    shader = tmp_path / f"linked_{tmp_path.name}.slang"
    try:
        shader.symlink_to(source)
    except OSError:
        pytest.skip("Symbolic links are unavailable")
    source.with_suffix(".slang.json").write_text('{"ShaderParams":{"value":{"value":1}}}')
    shader.with_suffix(".slang.json").write_text('{"ShaderParams":{"value":{"value":2}}}')
    scene = app.scene("smoke")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    scene.set_shader("nanovdb", shader)
    assert struct.unpack_from("=f", shader_state(app)[1])[0] == 2


@pytest.mark.parametrize("wrap", [bytes, bytearray, memoryview, lambda data: np.frombuffer(data, dtype=np.uint32)])
def test_repeated_buffer_add_preserves_material_and_pipelines(app, wrap):
    scene = app.scene("streaming")
    with scene.nanovdb_from_buffer(raw_empty_grid(), shader="editor/wireframe.slang",
                                   shader_parameters={"highlight_bbox": 1}):
        pass
    scene.set_pipeline("nanovdb", nve.PipelineStage.PROCESS, "voxelbvh")
    expected_material = shader_state(app, "streaming")
    source = raw_empty_grid()
    source[40:45] = b"frame"
    expected_buffer = bytes(source)
    with scene.nanovdb_from_buffer(wrap(source)) as grid:
        source[:] = b"\0" * len(source)
        assert grid.to_numpy().tobytes() == expected_buffer
    assert shader_state(app, "streaming") == expected_material
    assert scene.get_pipeline("nanovdb", nve.PipelineStage.PROCESS).type_id == "voxelbvh_build"
    assert scene.get_render_pipeline("nanovdb").type_id == "nanovdb_render"


def test_remove_and_add_buffer_restores_default_material(app):
    scene = app.scene("streaming")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    defaults = shader_state(app, "streaming")
    scene.set_shader("nanovdb", "editor/wireframe.slang", parameters={"highlight_bbox": 1})
    scene.remove("nanovdb")
    with scene.nanovdb_from_buffer(raw_empty_grid()):
        pass
    assert shader_state(app, "streaming") == defaults
