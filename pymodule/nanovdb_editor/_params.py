# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Scoped access to the editor's reflected custom scene parameters."""

import ctypes as ct
import math
import numbers
from collections.abc import MutableMapping


class _ReflectType(ct.Structure):
    pass


class _ReflectField(ct.Structure):
    _fields_ = [
        ("hint", ct.c_uint32),
        ("mode", ct.c_uint32),
        ("data_type", ct.POINTER(_ReflectType)),
        ("name", ct.c_char_p),
        ("offset", ct.c_uint64),
        ("array_size_offset", ct.c_uint64),
        ("version_offset", ct.c_uint64),
        ("metadata", ct.c_char_p),
    ]


_ReflectType._fields_ = [
    ("kind", ct.c_uint32),
    ("size", ct.c_uint64),
    ("name", ct.c_char_p),
    ("fields", ct.POINTER(_ReflectField)),
    ("field_count", ct.c_uint64),
    ("default", ct.c_void_p),
]

# Reflect.h scalar type identifiers.
_SCALARS = {
    4: ct.c_int32,
    5: ct.c_uint32,
    6: ct.c_float,
    7: ct.c_int32,
    8: ct.c_uint8,
    9: ct.c_uint16,
    10: ct.c_uint64,
    11: ct.c_char,
    12: ct.c_double,
    13: ct.c_int64,
}


class MappedParams(MutableMapping):
    """A fixed-schema mapping that becomes invalid when its context exits."""

    def __init__(self, address, data_type):
        self._address = address
        self._fields = {}
        reflected = ct.cast(data_type, ct.POINTER(_ReflectType)).contents
        for index in range(reflected.field_count):
            field = reflected.fields[index]
            scalar = field.data_type.contents
            ctype = _SCALARS.get(scalar.kind)
            if field.mode != 0 or ctype is None or not scalar.size or scalar.size % ct.sizeof(ctype):
                raise TypeError("Unsupported custom parameter layout")
            if field.offset + scalar.size > reflected.size:
                raise ValueError("Custom parameter exceeds its buffer")
            self._fields[field.name.decode("utf-8")] = (
                int(field.offset), ctype, int(scalar.size // ct.sizeof(ctype)), int(scalar.kind)
            )

    def close(self):
        self._address = None

    def _require_open(self):
        if self._address is None:
            raise RuntimeError("Custom parameters are only valid inside their context")

    def __len__(self):
        self._require_open()
        return len(self._fields)

    def __iter__(self):
        self._require_open()
        return iter(self._fields)

    def __getitem__(self, key):
        self._require_open()
        offset, ctype, count, kind = self._fields[key]
        values = (ctype * count).from_address(self._address + offset)
        if kind == 11:
            return bytes(values).split(b"\0", 1)[0].decode("utf-8", errors="replace")
        copied = tuple(bool(value) if kind == 7 else value for value in values)
        return copied[0] if count == 1 else copied

    def __setitem__(self, key, value):
        self._require_open()
        offset, ctype, count, kind = self._fields[key]
        if kind == 11:
            if not isinstance(value, str) or "\0" in value:
                raise TypeError("String parameters require text without null bytes")
            encoded = value.encode("utf-8")
            if len(encoded) >= count:
                raise ValueError(f"String parameter requires fewer than {count} UTF-8 bytes")
            ct.memset(self._address + offset, 0, count)
            ct.memmove(self._address + offset, encoded, len(encoded))
            return
        values = (value,) if count == 1 else tuple(value)
        if len(values) != count:
            raise ValueError(f"Parameter requires {count} values")
        converted = [self._convert(item, ctype, kind) for item in values]
        (ctype * count).from_address(self._address + offset)[:] = converted

    def __delitem__(self, key):
        self._require_open()
        raise TypeError("Custom parameter fields cannot be deleted")

    @staticmethod
    def _convert(value, ctype, kind):
        if kind == 7:
            if not isinstance(value, bool):
                raise TypeError("Boolean parameters require bool values")
            return int(value)
        if kind in (6, 12):
            if not isinstance(value, numbers.Real):
                raise TypeError("Floating-point parameters require real numbers")
            converted = ctype(value).value
            if not math.isfinite(converted):
                raise ValueError("Floating-point parameters require finite values")
            return converted
        if not isinstance(value, numbers.Integral):
            raise TypeError("Integer parameters require integer values")
        converted = ctype(int(value)).value
        if converted != value:
            raise ValueError("Integer parameter is outside its representable range")
        return converted
