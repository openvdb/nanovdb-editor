# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
import json
from ctypes import pointer
from base64 import b64encode
from contextlib import closing
from http.client import HTTPConnection
from threading import Event, Thread
from time import monotonic, sleep
from urllib.request import urlopen

import pytest

import nanovdb_editor as nve


pytestmark = pytest.mark.skipif(
    os.environ.get("NANOVDB_EDITOR_SKIP_STREAMING_SMOKE_TESTS", "0") == "1",
    reason="Streaming smoke tests are disabled on this runner",
)


@pytest.mark.parametrize("profile,title,custom_controls", [
    ("default", "NanoVDB Editor", True),
    ("viewer", "NanoVDB Editor - fVDB", False),
    ("viewer", "NanoVDB Editor - fVDB", True),
    ("nvflow", "NanoVDB Editor - NvFlow", False),
    ("nvflow", "NanoVDB Editor - NvFlow", True),
])
def test_streamed_profile_title_and_viewer_startup_camera(tmp_path, monkeypatch, profile, title, custom_controls):
    monkeypatch.chdir(tmp_path)
    stale_settings = (
        "[RenderSettings][default]\nui_profile_name=viewer\nis_y_up=0\n"
        "[CameraState][default]\nposition=100,200,300\neye_distance_from_position=1\n"
    )
    ini = tmp_path / ("imgui_nvflow.ini" if profile == "nvflow" else "imgui_viewer.ini")
    ini.write_text(stale_settings)

    with nve.create_default() as app:
        scene = app.scene("flow-stage")
        if custom_controls:
            scene.set_custom_params({"SceneParams": {
                "Play": {"type": "bool", "value": True, "widget": "toggleButton", "group": "Simulation"},
                "Restart": {"type": "bool", "value": False, "widget": "button"},
                "Other": {"type": "bool", "value": False, "widget": "button", "group": "Other"},
            }})
        camera = scene.update_camera(
            position=(5, 3, 2), eye_direction=(0, 0, -1), eye_up=(0, 1, 0), eye_distance=12
        )
        camera.config.is_orthographic = 1
        scene.update_camera(camera)
        camera_view = nve.CameraView()
        camera_view.name = app.editor.get_token("camera")
        camera_view.num_cameras = 1
        camera_view.configs = pointer(camera.config)
        camera_view.states = pointer(camera.state)
        app.editor.add_camera_view_2(scene.token, camera_view)
        app.start(headless=True, streaming=True, ip="127.0.0.1", ui_profile=profile)
        port = app.editor.get_resolved_port(wait=True)
        assert port > 0
        address = f"http://127.0.0.1:{port}"
        with closing(HTTPConnection("127.0.0.1", port, timeout=10)) as stream:
            stream.request("GET", "/ws", headers={
                "Connection": "Upgrade",
                "Upgrade": "websocket",
                "Sec-WebSocket-Version": "13",
                "Sec-WebSocket-Key": b64encode(os.urandom(16)).decode(),
            })
            assert stream.getresponse().status == 101
            for _ in range(2):
                deadline = monotonic() + 30
                while True:
                    with urlopen(address + "/screenshot.png", timeout=30) as response:
                        if response.read().startswith(b"\x89PNG\r\n\x1a\n"):
                            break
                    assert monotonic() < deadline, "The editor did not produce a frame"
            with urlopen(address, timeout=10) as response:
                assert f"<title>{title}</title>" in response.read().decode()
            scene_file = tmp_path / "scenes.json"
            app.save_scene(scene_file)
            scene_names = {item["name"] for item in json.loads(scene_file.read_text())["scenes"]}
            expected_scenes = {"flow-stage", "default"} if profile == "default" else {"flow-stage"}
            assert scene_names == expected_scenes
            if profile in ("viewer", "nvflow"):
                camera = scene.get_camera()
                assert camera is not None
                assert (camera.state.position.x, camera.state.position.y, camera.state.position.z) == (5, 3, 2)
                assert camera.state.eye_distance_from_position == 12
                assert camera.config.is_orthographic == 1
                assert (camera.state.eye_direction.x, camera.state.eye_direction.y, camera.state.eye_direction.z) == (0, 0, -1)
    assert ini.read_text() == stale_settings


def test_buffer_update_wakes_stream_without_browser(tmp_path, monkeypatch):
    from test_nanovdb_buffer import raw_empty_grid

    monkeypatch.chdir(tmp_path)
    with nve.create_default() as app:
        scene = app.scene("inactive-stream")
        with scene.nanovdb_from_buffer(raw_empty_grid()):
            pass
        app.start(headless=True, streaming=True, ip="127.0.0.1", ui_profile="viewer")
        assert app.editor.get_resolved_port(wait=True) > 0
        sleep(0.5)
        completed = Event()
        errors = []

        def replace_buffer():
            try:
                with scene.nanovdb_from_buffer(raw_empty_grid()):
                    pass
            except Exception as error:
                errors.append(error)
            finally:
                completed.set()

        thread = Thread(target=replace_buffer, daemon=True)
        thread.start()
        finished_without_browser = completed.wait(5)
        if not finished_without_browser:
            app.stop()
        thread.join(5)
        assert not thread.is_alive()
        assert finished_without_browser, "The inactive stream did not wake for a queued buffer update"
        assert not errors
