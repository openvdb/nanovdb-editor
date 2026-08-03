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
        with pytest.raises(nve.PipelineError):
            self.editor.set_custom_scene_params(scene, "{ this is not valid json ")

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
