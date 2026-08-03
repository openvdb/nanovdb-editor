# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Lifecycle tests for :func:`nanovdb_editor.create_default` / :class:`Session`."""

import inspect

import pytest

import nanovdb_editor as nve


class TestSessionLifecycle:
    def test_unpack_and_attributes(self):
        session = nve.create_default(device=False)
        try:
            editor, compute, compiler = session
            assert editor is session.editor
            assert compute is session.compute
            assert compiler is session.compiler
            assert len(session) == 3
            assert session[0] is editor
        finally:
            session.close()

    def test_close_is_idempotent(self):
        session = nve.create_default(device=False)
        session.close()
        session.close()  # must not raise
        assert session.editor is None
        assert session.compute is None
        assert session.compiler is None
        with pytest.raises(nve.SessionClosedError, match="closed"):
            session.scene("main")

    def test_session_forwards_and_config_kwargs(self):
        session = nve.create_default(device=False)
        try:
            assert hasattr(session, "wait_for_interrupt")
            assert hasattr(session, "load_scene")
            assert hasattr(session, "save_scene")
            assert hasattr(session, "get_resolved_port")
            assert hasattr(session, "run")
            cfg = nve.make_editor_config(ip="127.0.0.1", port=18080, headless=True)
            assert cfg.headless == 1
        finally:
            session.close()

    def test_failed_shutdown_keeps_compiler_alive(self):
        """A failing native shutdown must not be reported as a completed teardown."""
        session = nve.create_default(device=False)
        editor = session.editor
        real_handle = editor._editor

        class _FailingHandle:
            @property
            def contents(self):
                raise OSError("native shutdown unavailable")

        editor._editor = _FailingHandle()
        assert editor.shutdown() is False
        assert editor.close() is False

        # The compiler is still wired to a native editor of unknown state.
        assert session.close() is False
        assert session.compiler is None
        assert session._pending_teardown is not None

        # Once the native side responds again, close() finishes the teardown.
        editor._editor = real_handle
        assert session.close() is True
        assert session._pending_teardown is None

    def test_compiler_destroy_keeps_handle_on_failure(self):
        """A failed native destroy must stay retryable instead of dropping the handle."""
        session = nve.create_default(device=False)
        compiler = session.compiler
        instance = compiler._instance
        assert instance

        class _FailingCompiler:
            @property
            def contents(self):
                raise OSError("native destroy unavailable")

        real_compiler = compiler._compiler
        compiler._compiler = _FailingCompiler()
        with pytest.raises(OSError):
            compiler.destroy_instance()
        assert compiler._instance is instance

        compiler._compiler = real_compiler
        compiler.destroy_instance()
        assert compiler._instance is None
        session.close()

    def test_close_reports_failure_when_compiler_teardown_fails(self):
        """A compiler that could not be destroyed must not be reported as closed."""
        session = nve.create_default(device=False)
        compiler = session.compiler
        attempts = []

        def _failing_destroy():
            attempts.append(True)
            raise RuntimeError("destroy_instance failed")

        compiler.destroy_instance = _failing_destroy
        assert session.close() is False
        assert attempts == [True]
        assert session._pending_teardown == (None, compiler)

        del compiler.destroy_instance
        assert session.close() is True
        assert session._pending_teardown is None

    def test_wait_for_interrupt_is_noop_after_editor_close(self):
        session = nve.create_default(device=False)
        editor = session.editor
        editor.close()
        editor.wait_for_interrupt()
        session.close()

    def test_stop_reaches_editor_awaiting_teardown(self):
        """stop() must still reach a worker left running by an incomplete close()."""
        session = nve.create_default(device=False)
        editor = session.editor
        real_handle = editor._editor

        class _FailingHandle:
            @property
            def contents(self):
                raise OSError("native shutdown unavailable")

        editor._editor = _FailingHandle()
        assert session.close() is False
        assert session.editor is None

        stopped = []
        editor.stop = lambda: stopped.append(True)
        session.stop()
        assert stopped == [True], "stop() no-opped while the editor was still alive"

        waited = []
        editor.wait_for_interrupt = lambda: waited.append(True)
        session.wait_for_interrupt()
        assert waited == [True], "wait_for_interrupt() must reach the surviving editor"

        editor._editor = real_handle
        assert session.close() is True

    def test_lazy_device_uses_requested_device_id(self):
        """A device created lazily must land on the device_id the caller asked for."""
        session = nve.create_default(device_id=3, device=False)
        try:
            editor = session.editor
            device_interface = session.compute.device_interface()
            created = {}
            device_interface.create_device_manager = lambda *a, **kw: None
            device_interface.create_device = lambda **kw: created.update(kw)

            editor._ensure_device()

            assert created.get("device_index") == 3
        finally:
            session.close()

    def test_get_resolved_port_accepts_positional_flag(self):
        # Older callers pass the flag positionally; that must keep binding.
        for target in (nve.Editor.get_resolved_port, nve.Session.get_resolved_port):
            sig = inspect.signature(target)
            sig.bind(None, True)
            sig.bind(None, wait=True)

    def test_context_manager_closes(self):
        with nve.create_default(device=False) as session:
            assert session.editor is not None
            scene = session.scene("main")
            assert scene is not None
        assert session.editor is None
        # Double-close via __exit__ then explicit close must remain safe.
        session.close()

    def test_editor_close_is_idempotent(self):
        session = nve.create_default(device=False)
        editor = session.editor
        assert editor.close() is True
        assert editor.close() is True  # must not raise
        assert editor.shutdown() is True  # must not raise
        editor.stop()  # must not raise
        assert session.close() is True

    def test_close_keeps_handle_when_teardown_deferred(self):
        """A deferred native teardown must stay retryable, not drop the handle."""
        session = nve.create_default(device=False)
        editor = session.editor

        # Native shutdown() defers teardown (leaves impl alive) when called from
        # the active render thread; emulate that without a running render loop.
        deferred = {"value": True}
        editor.shutdown = lambda: not deferred["value"]

        assert session.close() is False
        assert editor._editor is not None, "handle must survive a deferred teardown"
        assert session.compiler is None

        deferred["value"] = False
        assert session.close() is True
        assert editor._editor is None
