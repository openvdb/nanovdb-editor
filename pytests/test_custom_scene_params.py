# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""
Integration tests for the scene-level custom params Python API:
    set_custom_scene_params, set_custom_scene_params_from_file,
    get_custom_scene_params_data_type, map_params/unmap_params (name=None),
    and JSON hot-reload via reload_custom_scene_params_if_changed.
"""

import gc
import json
import os
import time

import pytest

import nanovdb_editor as nve  # type: ignore


CUSTOM_PARAMS_JSON = {
    "SceneParams": {
        "gain": {"type": "float", "value": 1.5, "min": 0.0, "max": 5.0, "step": 0.25},
        "toggle": {"type": "bool", "value": True},
        "offset": {"type": "int", "value": [1, 2, 3], "elementCount": 3, "useSlider": True},
        "prompt": {"type": "string", "length": 32, "value": "a red chair"},
    }
}


class TestCustomSceneParams:
    @pytest.fixture(autouse=True)
    def setup_and_teardown(self):
        self.session = nve.create_default()
        self.editor, self.compute, self.compiler = self.session

        yield

        self.session.close()
        self.editor = None
        self.compute = None
        self.compiler = None
        gc.collect()

    def test_set_custom_scene_params(self):
        scene = self.editor.get_token("custom_scene")

        self.editor.set_custom_scene_params(scene, json.dumps(CUSTOM_PARAMS_JSON))

        # A reflected data-type handle is available once params are attached.
        data_type = self.editor.get_custom_scene_params_data_type(scene)
        assert data_type, "Expected a non-null custom scene params data type handle"

        # The handle round-trips through the public map/unmap params path.
        address = self.editor.map_params(scene, None, data_type)
        try:
            assert address, "map_params should return a valid buffer for custom scene params"
        finally:
            self.editor.unmap_params(scene, None)

    def test_invalid_json_raises(self):
        scene = self.editor.get_token("bad_scene")
        with pytest.raises(nve.PipelineError, match=".+") as exc_info:
            self.editor.set_custom_scene_params(scene, "{ this is not valid json ")
        # Error text must come from the native error_buf (ABI write path).
        assert exc_info.value.args[0].strip()

        # Unknown scene has no params attached.
        empty_scene = self.editor.get_token("empty_scene")
        assert not self.editor.get_custom_scene_params_data_type(empty_scene)

    def test_set_from_file_and_hot_reload(self, tmp_path):
        scene = self.editor.get_token("file_scene")
        json_path = tmp_path / "scene_params.json"
        json_path.write_text(json.dumps(CUSTOM_PARAMS_JSON))

        self.editor.set_custom_scene_params_from_file(scene, str(json_path))
        assert self.editor.get_custom_scene_params_data_type(scene)

        # No change yet -> no reload.
        assert self.editor.reload_custom_scene_params_if_changed(scene) is False

        # Modify the file and force a newer mtime, then confirm a reload happens.
        updated = json.loads(json.dumps(CUSTOM_PARAMS_JSON))
        updated["SceneParams"]["gain"]["value"] = 3.75
        json_path.write_text(json.dumps(updated))
        future = time.time() + 2
        os.utime(json_path, (future, future))

        assert self.editor.reload_custom_scene_params_if_changed(scene) is True
        assert self.editor.reload_custom_scene_params_if_changed(scene) is False


@pytest.fixture
def custom_scene():
    with nve.create_default(device=False) as session:
        scene = session.scene("controls")
        scene.set_custom_params({"SceneParams": {
            "Play": {"type": "bool", "value": True, "group": "Settings"},
            "Frame": {"type": "uint", "value": 12, "readOnly": True},
            "Time (s)": {"type": "float", "value": 0.5},
            "Light direction": {"type": "float", "value": [1.0, 2.0, 3.0]},
            "Stage": {"type": "string", "value": "smoke", "length": 16},
        }})
        yield scene


def test_scene_control_mapping_round_trip(custom_scene):
    with custom_scene.custom_params() as params:
        assert dict(params) == {
            "Play": True, "Frame": 12, "Time (s)": 0.5,
            "Light direction": (1.0, 2.0, 3.0), "Stage": "smoke",
        }
        params["Play"] = False
        params["Frame"] = 13
        params["Time (s)"] = 0.75
        params["Light direction"] = (3.0, 2.0, 1.0)
        params["Stage"] = "plume"
    with custom_scene.custom_params() as params:
        assert params["Play"] is False
        assert params["Frame"] == 13
        assert params["Time (s)"] == 0.75
        assert params["Light direction"] == (3.0, 2.0, 1.0)
        assert params["Stage"] == "plume"


def test_scene_controls_release_after_exception(custom_scene):
    with pytest.raises(RuntimeError, match="application error"):
        with custom_scene.custom_params() as params:
            params["Frame"] = 99
            raise RuntimeError("application error")
    with pytest.raises(RuntimeError, match="inside their context"):
        _ = params["Frame"]
    with pytest.raises(RuntimeError, match="inside their context"):
        params["Frame"] = 0
    with custom_scene.custom_params() as reopened:
        assert reopened["Frame"] == 99


@pytest.mark.parametrize("key,value,error", [
    ("Play", 1, TypeError),
    ("Frame", -1, ValueError),
    ("Frame", 2**32, ValueError),
    ("Frame", 1.5, TypeError),
    ("Time (s)", float("nan"), ValueError),
    ("Time (s)", 1e100, ValueError),
    ("Light direction", [1.0, 2.0], ValueError),
    ("Light direction", [1.0, float("inf"), 3.0], ValueError),
    ("Stage", "x" * 16, ValueError),
    ("Stage", "null\0byte", TypeError),
    ("missing", 0, KeyError),
])
def test_scene_controls_reject_invalid_values(custom_scene, key, value, error):
    with custom_scene.custom_params() as params:
        before = dict(params)
        with pytest.raises(error):
            params[key] = value
        assert dict(params) == before


def test_empty_scene_controls_raise():
    with nve.create_default(device=False) as session:
        with pytest.raises(nve.PipelineError, match="no custom parameters"):
            with session.scene("empty").custom_params():
                pass


def test_schema_reload_waits_for_mapped_context(custom_scene):
    import threading

    started = threading.Event()
    reloaded = threading.Event()
    errors = []

    def reload_schema():
        started.set()
        try:
            custom_scene.set_custom_params({"SceneParams": {
                "New field": {"type": "uint64", "value": 2**40},
            }})
        except Exception as error:
            errors.append(error)
        finally:
            reloaded.set()

    with custom_scene.custom_params() as params:
        thread = threading.Thread(target=reload_schema)
        thread.start()
        assert started.wait(5)
        blocked = not reloaded.wait(0.1)
        assert params["Frame"] == 12
    thread.join(5)
    assert not thread.is_alive()
    assert not errors
    assert blocked, "Schema replacement must wait for the mapped context"
    with custom_scene.custom_params() as params:
        assert dict(params) == {"New field": 2**40}
