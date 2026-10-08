# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Prepare complete shader values for the editor's parameter mapping API."""

import ctypes as ct
import json
import math
from pathlib import Path
import struct
import tempfile

from ._params import _ReflectField, _ReflectType
from .editor import EditorToken
from .exceptions import PipelineError


SHADER_PARAMS_SIZE = 65536


class Shader(ct.Structure):
    _fields_ = [
        ("shader_name", ct.POINTER(EditorToken)),
        ("shader_params", ct.c_uint8 * SHADER_PARAMS_SIZE),
    ]


def _struct_type(ctype, name, fields):
    reflected = (_ReflectField * len(fields))(
        *[
            _ReflectField(0, mode, ct.pointer(data_type), field.encode(), getattr(ctype, field).offset, 0, 0, None)
            for field, data_type, mode in fields
        ]
    )
    return _ReflectType(3, ct.sizeof(ctype), name.encode(), reflected, len(fields), None)


_UINT64 = _ReflectType(10, 8)
_CHAR = _ReflectType(11, 1)
_TOKEN = _struct_type(EditorToken, "pnanovdb_editor_token_t", [("id", _UINT64, 0), ("str", _CHAR, 1)])
_BUFFER = _ReflectType(0, SHADER_PARAMS_SIZE)
SHADER_TYPE = _struct_type(
    Shader, "pnanovdb_editor_shader_t", [("shader_name", _TOKEN, 1), ("shader_params", _BUFFER, 0)]
)

_FORMATS = {
    "int": "i",
    "uint": "I",
    "int64": "q",
    "uint64": "Q",
    "float16": "e",
    "half": "e",
    "float": "f",
    "double": "d",
    "bool": "I",
    "": "f",
    "void": "f",
}


def _pack_scalar(format_code, value, boolean, strict):
    if isinstance(value, bool):
        if strict and not boolean:
            raise ValueError("Boolean value requires a boolean field")
        # Native JSON array defaults reject booleans for double.
        if not strict and format_code == "d":
            raise ValueError("Expected a number")
        value = int(value)
    if not isinstance(value, (int, float)):
        raise ValueError("Expected a number")
    if format_code in "efd":
        if format_code == "e":
            value = struct.unpack("=f", struct.pack("=f", value))[0]
        packed = struct.pack("=" + format_code, value)
        if not math.isfinite(struct.unpack("=" + format_code, packed)[0]):
            raise ValueError("Expected a finite value")
        return packed
    if strict and not isinstance(value, int):
        raise ValueError("Expected an integer")
    value = int(value)
    if not strict:
        bits = 8 * struct.calcsize("=" + format_code)
        value &= (1 << bits) - 1
        if format_code in "iq" and value >= 1 << (bits - 1):
            value -= 1 << bits
    return struct.pack("=" + format_code, value)


def pack_shader_parameters(compiler, shader, parameters):
    shader_dir = Path(compiler._lib._name).parent.parent / "shaders"
    cache_dir = shader_dir / "_generated"
    if not cache_dir.is_dir():
        cache_dir = Path(tempfile.gettempdir()) / "nanovdb_editor" / "shader_cache"
    try:
        reflection = json.loads((cache_dir / (Path(shader).name + ".json")).read_text())
        fields = reflection.get("ShaderParams", reflection.get("shaderParams")) or {}
        if not isinstance(fields, dict):
            raise ValueError("Invalid shader reflection")
    except (OSError, ValueError, AttributeError) as exc:
        raise PipelineError("Shader reflection is unavailable; compile the shader first") from exc

    source = Path(shader)
    if not source.exists():
        source = (shader_dir / source).resolve()
    try:
        hints = json.loads(Path(str(source) + ".json").read_text()).get("ShaderParams", {})
        if not isinstance(hints, dict):
            hints = {}
    except (OSError, ValueError, AttributeError):
        hints = {}

    fields = {name: field for name, field in fields.items() if "_pad" not in name}
    unknown = parameters.keys() - fields.keys()
    if unknown:
        raise PipelineError(f"Unknown shader parameter: {next(iter(unknown))}")

    result = bytearray(SHADER_PARAMS_SIZE)
    offset = 0
    for name, field in fields.items():
        try:
            scalar_type = field["type"]
            format_code = _FORMATS[scalar_type]
            count = 1 if scalar_type == "bool" else field["elementCount"]
            if not isinstance(count, int) or count < 1:
                raise ValueError("Invalid element count")
            size = struct.calcsize("=" + format_code) * count
            if size > len(result) - offset:
                raise ValueError("Shader parameters exceed the constant buffer")
            hint = hints.get(name, {})
            if not isinstance(hint, dict):
                hint = {}
            boolean = scalar_type == "bool" or (
                count == 1 and format_code in "iIqQd" and hint.get("isBool") is True
            )
            strict = name in parameters
            value = parameters.get(name, hint.get("value", 0))
            if scalar_type == "bool":
                if strict and not isinstance(value, bool):
                    raise ValueError("Expected a boolean")
                value = int(value) if isinstance(value, bool) else 0
            elif not strict and (isinstance(value, bool) or not isinstance(value, (int, float, list))):
                value = 0
            if not strict:
                values = value if isinstance(value, list) else [value]
                values = (values + [0] * count)[:count]
            elif count == 1:
                values = [value]
            elif isinstance(value, list):
                values = value
            else:
                raise ValueError("Expected a vector")
            if len(values) != count:
                raise ValueError("Wrong vector length")
            packed = b"".join(_pack_scalar(format_code, item, boolean, strict) for item in values)
            result[offset : offset + size] = packed
            offset += size
        except (KeyError, TypeError, ValueError, OverflowError, struct.error) as exc:
            raise PipelineError(f"Invalid shader parameter: {name}") from exc
    return bytes(result)
