#!/usr/bin/env python3
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
# pylint: disable=import-error,no-member,protected-access

import gc
import os
import platform
from pathlib import Path
from time import sleep

import pytest

import nanovdb_editor as nve  # type: ignore


def _windows_h264_enabled() -> bool:
    return os.environ.get("NANOVDB_EDITOR_WINDOWS_H264", "0") == "1"


def _contains_h264_start_code(payload: bytes) -> bool:
    return b"\x00\x00\x00\x01" in payload or b"\x00\x00\x01" in payload


@pytest.mark.skipif(
    platform.system() == "Windows" and not _windows_h264_enabled(),
    reason="Windows H264 test requires a vcpkg-backed OpenH264-enabled build",
)
def test_editor_stream_to_file_produces_h264_output(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)

    if "VK_ICD_FILENAMES" in os.environ or "VK_DRIVER_FILES" in os.environ:
        print(
            f"Using Vulkan ICD: {os.environ.get('VK_ICD_FILENAMES', 'N/A')}"
        )
        print(
            f"Using Vulkan driver: {os.environ.get('VK_DRIVER_FILES', 'N/A')}"
        )

    compiler = nve.Compiler()
    compiler.create_instance()

    compute = nve.Compute(compiler)
    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    editor = nve.Editor(compute, compiler)

    config = nve.EditorConfig()
    config.ip_address = b"127.0.0.1"
    config.port = 8080
    config.headless = 1
    config.streaming = 1
    config.stream_to_file = 1
    config.ui_profile_name = None

    output_path = Path(tmp_path) / "capture_stream.h264"

    try:
        print(
            f"Starting editor on platform={platform.system()} "
            f"streaming={config.streaming} "
            f"stream_to_file={config.stream_to_file} "
            f"headless={config.headless} preferred_port={config.port}"
        )
        compiler.clear_diagnostics()
        editor.start(config)
        resolved_port = editor.get_resolved_port(True)
        assert resolved_port > 0, "Expected the editor to resolve a streaming port"
        print(f"Editor resolved streaming port={resolved_port}")

        # Allow the render loop to produce and flush a few encoded frames.
        sleep(1.5)

        diagnostics = compiler.get_diagnostics()
        if diagnostics:
            print(f"Compiler diagnostics during startup:\n{diagnostics}")
    except Exception as exc:
        raise AssertionError(
            "Editor CPU streaming to file failed.\n"
            f"Compiler diagnostics:\n{compiler.get_diagnostics() or '<none>'}"
        ) from exc
    finally:
        editor.stop()
        editor = None
        compute = None
        compiler._instance = None  # noqa: SLF001
        compiler._compiler = None  # noqa: SLF001
        compiler = None
        gc.collect()

    assert output_path.exists(), (
        f"Expected H264 output file to exist: {output_path}"
    )

    payload = output_path.read_bytes()
    assert payload, "Expected non-empty H264 output file"
    assert len(payload) > 128, (
        "Expected H264 output larger than a trivial header, "
        f"got {len(payload)} bytes"
    )
    assert _contains_h264_start_code(payload), (
        "Expected H264 Annex B start codes in encoded output"
    )
