# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from contextlib import nullcontext
from ctypes import POINTER, addressof, pointer

import pytest

from nanovdb_editor.editor import Editor, pnanovdb_PipelineParams


@pytest.mark.parametrize("mapping", ["params", "pipeline_params", "process_step_params"])
@pytest.mark.parametrize("body_error", [False, True])
def test_failed_nested_mapping_unmaps_only_outer(monkeypatch, mapping, body_error):
    editor = Editor.__new__(Editor)
    payload = pnanovdb_PipelineParams()
    mapped = addressof(payload) if mapping == "params" else pointer(payload)
    missing = None if mapping == "params" else POINTER(pnanovdb_PipelineParams)()
    results = iter([mapped, missing])
    unmap_calls = []
    monkeypatch.setattr(editor, "map_" + mapping, lambda *args: next(results))
    monkeypatch.setattr(editor, "unmap_" + mapping, lambda *args: unmap_calls.append(args))
    context = getattr(editor, mapping)
    outer_args = ("scene", "object", 0)

    expected_error = pytest.raises(RuntimeError, match="inner mapping failed") if body_error else nullcontext()
    with expected_error:
        with context(*outer_args) as outer:
            assert outer is not None
            with context("scene", "missing", 0) as inner:
                assert inner is None
                if body_error:
                    raise RuntimeError("inner mapping failed")
            assert not unmap_calls
    unmap_args = outer_args[:2] if mapping == "params" else outer_args
    assert unmap_calls == [unmap_args]
