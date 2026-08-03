# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

from typing import Iterator, Optional

from .compiler import Compiler
from .compute import Compute
from .editor import Editor
from .exceptions import SessionClosedError
from .scene import Scene


class Session:
    """The core objects needed to build and view grids.

    Returned by :func:`create_default`. It unpacks exactly like the
    ``(editor, compute, compiler)`` tuple it replaces::

        editor, compute, compiler = nve.create_default()

    but also supports attribute access, forwards the common editor calls, and
    can be used as a context manager that performs a guarded native shutdown
    on exit::

        with nve.create_default() as app:
            app.scene("main").nanovdb_from_gaussians(...)
            app.show()
        # editor stopped + shut down; compiler instance destroyed

    ``show`` runs a blocking GUI. ``start`` launches a non-blocking worker
    (typically headless/streaming). ``run`` is a convenience wrapper: GUI by
    default, or ``run(gui=False, headless=True, ...)`` for start+wait.
    """

    def __init__(self, editor: Editor, compute: Compute, compiler: Compiler):
        self.editor: Optional[Editor] = editor
        self.compute: Optional[Compute] = compute
        self.compiler: Optional[Compiler] = compiler
        self._closed = False
        # Holds (editor, compiler) when native teardown was deferred, so a later
        # close() can finish it instead of leaking the still-running editor.
        self._pending_teardown = None

    def _require_editor(self) -> Editor:
        if self.editor is None:
            raise SessionClosedError("Session has been closed")
        return self.editor

    def _live_editor(self) -> Optional[Editor]:
        """The editor to act on, including one awaiting a deferred teardown."""
        if self.editor is not None:
            return self.editor
        if self._pending_teardown is not None:
            return self._pending_teardown[0]
        return None

    def __iter__(self) -> Iterator:
        """Unpack as ``(editor, compute, compiler)``."""
        yield self.editor
        yield self.compute
        yield self.compiler

    def __len__(self) -> int:
        return 3

    def __getitem__(self, index: int):
        return (self.editor, self.compute, self.compiler)[index]

    def scene(self, name: str) -> Scene:
        """Shortcut for ``self.editor.scene(name)``."""
        return self._require_editor().scene(name)

    def show(self, config=None, **kwargs):
        """Run the editor UI (blocking). See :meth:`Editor.show`."""
        return self._require_editor().show(config, **kwargs)

    def start(self, config=None, **kwargs):
        """Start the editor worker (non-blocking). See :meth:`Editor.start`."""
        return self._require_editor().start(config, **kwargs)

    def run(self, *, gui: bool = True, **kwargs):
        """Convenience entry point: ``show`` when ``gui=True``, else ``start`` + wait.

        Example::

            app.run()  # interactive GUI
            app.run(gui=False, headless=True, streaming=True, ip="0.0.0.0", port=8080)
        """
        editor = self._require_editor()
        if gui:
            return editor.show(**kwargs)
        editor.start(**kwargs)
        editor.wait_for_interrupt()

    def stop(self) -> None:
        """Shortcut for ``self.editor.stop()``.

        Also reaches an editor whose teardown was deferred or failed, so a
        worker that outlived :meth:`close` can still be stopped before retrying.
        """
        editor = self._live_editor()
        if editor is not None:
            editor.stop()

    def wait_for_interrupt(self) -> None:
        """Block until the editor worker is interrupted.

        Like :meth:`stop`, this also reaches an editor whose teardown was
        deferred or failed, so callers can wait for the surviving render loop
        before retrying :meth:`close`. See :meth:`Editor.wait_for_interrupt`.
        """
        editor = self._live_editor()
        if editor is None:
            raise SessionClosedError("Session has been closed")
        editor.wait_for_interrupt()

    def load_scene(self, filepath, overwrite: bool = False) -> None:
        """Load a scene file. See :meth:`Editor.load_scene`."""
        self._require_editor().load_scene(filepath, overwrite=overwrite)

    def save_scene(self, filepath) -> None:
        """Save all scenes. See :meth:`Editor.save_scene`."""
        self._require_editor().save_scene(filepath)

    def get_resolved_port(self, should_wait=None, *, wait: bool = False) -> int:
        """Resolved streaming port. See :meth:`Editor.get_resolved_port`."""
        return self._require_editor().get_resolved_port(should_wait, wait=wait)

    def close(self) -> bool:
        """Stop the editor worker, shut down the native editor, destroy the compiler.

        Idempotent and exception-safe: never raises from teardown. After
        ``close``, attribute access still works (fields become ``None``) but
        ``scene`` / ``show`` / ``start`` raise :class:`SessionClosedError`.

        Returns:
            True when teardown completed. False when the native side deferred it
            because ``close`` ran on the active render thread, or when tearing
            down the editor or compiler failed — call ``close`` again once the
            render loop has exited.
        """
        if self._pending_teardown is not None:
            editor, compiler = self._pending_teardown
        elif self._closed:
            return True
        else:
            editor, compiler = self.editor, self.compiler

        self._closed = True
        # Drop public refs first so concurrent users see a closed session, then
        # tear down native state in a safe order: editor (stops worker + frees
        # impl) before the compiler instance it was wired to.
        self.editor = None
        self.compute = None
        self.compiler = None

        completed = True
        try:
            if editor is not None:
                completed = editor.close()
        except Exception:
            # Native state is unknown, so treat it like a deferred teardown
            # rather than freeing something the editor may still point at.
            completed = False

        if not completed:
            # The compiler must outlive the editor still wired to it.
            self._pending_teardown = (editor, compiler)
            return False

        try:
            if compiler is not None:
                compiler.destroy_instance()
        except Exception:
            # The editor is gone, so only the compiler is left to free; keep it
            # pending rather than reporting a teardown that did not happen.
            self._pending_teardown = (None, compiler)
            return False

        self._pending_teardown = None
        return True

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __repr__(self) -> str:
        if self._closed:
            return "Session(closed)"
        return f"Session(editor={self.editor!r})"


def create_default(device_id: int = 0, *, device: bool = True) -> Session:
    """Create ready-to-use editor, compute, and compiler instances.

    Bundles the full setup — creating the compiler and its instance, wiring up
    compute, optionally creating the device manager and device, and constructing
    the editor — so callers don't have to repeat the boilerplate.

    The returned :class:`Session` is a context manager; prefer::

        with nve.create_default() as app:
            ...

    so the native editor is shut down and the compiler instance destroyed on
    block exit. Calling :meth:`Session.close` explicitly is equivalent.

    Args:
        device_id: The Vulkan device index to use (default: 0). Also used if a
            device has to be created later, e.g. after ``device=False``.
        device: When ``True`` (default), create the device manager and device.
            Pass ``False`` for CPU-only / argument-validation setups that do
            not need a GPU.

    Returns:
        Session: a ``(editor, compute, compiler)`` bundle (see :class:`Session`).
    """
    compiler = Compiler()
    compiler.create_instance()

    compute = Compute(compiler)

    if device:
        device_interface = compute.device_interface()
        device_interface.create_device_manager()
        device_interface.create_device(device_index=device_id)

    editor = Editor(compute, compiler, device_index=device_id)

    return Session(editor, compute, compiler)
