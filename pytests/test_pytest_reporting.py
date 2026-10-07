# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
from pathlib import Path
import subprocess
import sys

import pytest


@pytest.mark.parametrize("test_body,plugin,exit_status,report", [
    ('assert False, "failure-details-sentinel"', "", 1, "1 failed"),
    ("pass", "def pytest_sessionfinish(session):\n    session.exitstatus = 3\n", 3, "1 passed"),
], ids=["failure-details", "plugin-exit-status"])
def test_session_reports_outcome(tmp_path, test_body, plugin, exit_status, report):
    (tmp_path / "conftest.py").write_text(Path(__file__).with_name("conftest.py").read_text())
    (tmp_path / "test_result.py").write_text(f"def test_result():\n    {test_body}\n")
    (tmp_path / "session_status.py").write_text(plugin)
    result = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "--color=no", "-p", "session_status"],
        cwd=tmp_path,
        env={**os.environ, "PYTEST_ADDOPTS": "", "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1"},
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == exit_status, result.stdout + result.stderr
    assert report in result.stdout
    if exit_status == 1:
        assert "failure-details-sentinel" in result.stdout
