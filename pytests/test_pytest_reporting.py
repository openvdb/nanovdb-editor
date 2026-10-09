# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import os
from pathlib import Path
import subprocess
import sys

import pytest


def run_pytest(tmp_path, test_body="pass", plugin="", arguments=()):
    (tmp_path / "conftest.py").write_text(Path(__file__).with_name("conftest.py").read_text())
    (tmp_path / "test_result.py").write_text(f"def test_result():\n    {test_body}\n")
    (tmp_path / "session_status.py").write_text(plugin)
    return subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "--color=no", "-p", "session_status", *arguments],
        cwd=tmp_path,
        env={**os.environ, "PYTEST_ADDOPTS": "", "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1"},
        capture_output=True,
        text=True,
        timeout=30,
    )


@pytest.mark.parametrize("test_body,plugin,exit_status,output", [
    ('assert False, "failure-details-sentinel"', "", 1, ("1 failed", "failure-details-sentinel")),
    ("pass", "def pytest_sessionfinish(session):\n    session.exitstatus = 3\n", 3, ("1 passed",)),
    ("pass", 'def pytest_sessionfinish(session):\n    raise RuntimeError("plugin-failure-sentinel")\n',
     1, ("Traceback", "RuntimeError: plugin-failure-sentinel")),
    ("pass", 'import pytest\ndef pytest_sessionfinish(session):\n    pytest.exit("plugin-exit-sentinel", returncode=7)\n',
     7, ("Exit: plugin-exit-sentinel",)),
    ("pass", 'import pytest\ndef pytest_sessionfinish(session):\n    pytest.exit("plugin-exit-sentinel")\n',
     0, ("Exit: plugin-exit-sentinel",)),
    ("assert False", 'import pytest\ndef pytest_sessionfinish(session):\n    pytest.exit("plugin-exit-sentinel")\n',
     1, ("Exit: plugin-exit-sentinel",)),
    ("pass", 'def pytest_sessionfinish(session):\n    raise SystemExit(7)\n', 7, ()),
], ids=["failure-details", "plugin-exit-status", "plugin-error", "plugin-exit-code",
        "plugin-exit-none-passed", "plugin-exit-none-failed", "plugin-system-exit"])
def test_session_reports_outcome(tmp_path, test_body, plugin, exit_status, output):
    result = run_pytest(tmp_path, test_body, plugin)
    assert result.returncode == exit_status, result.stdout + result.stderr
    for text in output:
        assert text in result.stdout + result.stderr


def test_startup_usage_error_preserves_exit_status(tmp_path):
    result = run_pytest(tmp_path, arguments=("--no-such-option-sentinel",))
    assert result.returncode == pytest.ExitCode.USAGE_ERROR, result.stdout + result.stderr
    assert "unrecognized arguments: --no-such-option-sentinel" in result.stderr
