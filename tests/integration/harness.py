"""Waiting on real processes and terminals with a deadline, never a sleep.

Everything here blocks only in select() or in a wait with a timeout, so a
hung child fails the test after a bounded time instead of hanging CI.
"""

from __future__ import annotations

import errno
import os
import pty
import select
import signal
import subprocess
import time
from dataclasses import dataclass

# How long any one step of the suite may take before it counts as a hang.
# Generous for an ASan build on a shared runner; a passing step takes far less.
STEP_TIMEOUT_SECONDS = 30.0

# How much one read takes off a descriptor.
READ_CHUNK = 64 * 1024


class Timeout(AssertionError):
    """A step outlived STEP_TIMEOUT_SECONDS."""


@dataclass
class Finished:
    """What a process left behind."""

    status: int
    output: bytes


# The first line the noisy .bashrc of conftest.py prints. Here rather than in
# conftest.py: a test module that imports conftest gets a second copy of it.
NOISE_BANNER = b"Welcome to the noisy host"


def _deadline(timeout: float) -> float:
    return time.monotonic() + timeout


def _remaining(deadline: float) -> float:
    return max(0.0, deadline - time.monotonic())


def read_until(fd: int, needle: bytes | None, timeout: float = STEP_TIMEOUT_SECONDS) -> bytes:
    """Reads `fd` until `needle` shows up, or to the end of the stream when it
    is None. A pty's end of stream is EIO on Linux and counts as one."""
    deadline = _deadline(timeout)
    seen = bytearray()
    while needle is None or needle not in seen:
        ready, _, _ = select.select([fd], [], [], _remaining(deadline))
        if not ready:
            raise Timeout(f"waited {timeout}s for {needle!r}, got {bytes(seen)[-400:]!r}")
        try:
            chunk = os.read(fd, READ_CHUNK)
        except OSError as error:
            if error.errno == errno.EIO:
                chunk = b""
            else:
                raise
        if not chunk:
            if needle is None:
                break
            raise AssertionError(f"stream ended before {needle!r}: {bytes(seen)[-400:]!r}")
        seen += chunk
    return bytes(seen)


def write_all(fd: int, data: bytes, timeout: float = STEP_TIMEOUT_SECONDS) -> None:
    """Writes all of `data`, waiting for room with a deadline."""
    deadline = _deadline(timeout)
    view = memoryview(data)
    while view:
        _, ready, _ = select.select([], [fd], [], _remaining(deadline))
        if not ready:
            raise Timeout(f"no room to write after {timeout}s, {len(view)} bytes left")
        view = view[os.write(fd, view):]


def wait_process(process: subprocess.Popen, timeout: float = STEP_TIMEOUT_SECONDS) -> int:
    """Waits for `process` to end; kills it and fails if it does not."""
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise Timeout(f"{process.args!r} still running after {timeout}s") from None


def _exits_within(pid: int, timeout: float) -> bool:
    """Whether `pid` ends within `timeout`, waiting on the kernel's own event
    for it: a pidfd on Linux, EVFILT_PROC on macOS, where the macOS client
    suite runs this too. No polling either way. A pid already gone counts as
    ended."""
    if hasattr(os, "pidfd_open"):
        try:
            pidfd = os.pidfd_open(pid)
        except ProcessLookupError:
            return True
        try:
            ready, _, _ = select.select([pidfd], [], [], timeout)
        finally:
            os.close(pidfd)
        return bool(ready)
    queue = select.kqueue()
    try:
        watch = select.kevent(pid, filter=select.KQ_FILTER_PROC,
                              flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                              fflags=select.KQ_NOTE_EXIT)
        try:
            return bool(queue.control([watch], 1, timeout))
        except ProcessLookupError:
            return True
    finally:
        queue.close()


def wait_pid(pid: int, timeout: float = STEP_TIMEOUT_SECONDS) -> int:
    """Reaps `pid`, waiting for its exit with a deadline; kills it if it is
    still running at the deadline."""
    ready = _exits_within(pid, timeout)
    if not ready:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        raise Timeout(f"pid {pid} still running after {timeout}s")
    _, status = os.waitpid(pid, 0)
    return status


def wait_gone(pid: int, timeout: float = STEP_TIMEOUT_SECONDS) -> None:
    """Waits for a process that is not our child to end: no polling, and no
    reaping, which is its parent's to do."""
    if not _exits_within(pid, timeout):
        raise Timeout(f"pid {pid} still running after {timeout}s")


# How often wait_until looks again. Used only where the kernel offers no
# event to wait on, such as another process closing one of its sockets.
POLL_INTERVAL_SECONDS = 0.05


def wait_until(condition, what: str, timeout: float = STEP_TIMEOUT_SECONDS) -> None:
    """Waits for `condition()` to hold, looking every POLL_INTERVAL_SECONDS up
    to the deadline. The one place the suite polls: keep it for state with no
    descriptor to wait on, and say in the caller why there is none."""
    deadline = _deadline(timeout)
    while not condition():
        if time.monotonic() >= deadline:
            raise Timeout(f"waited {timeout}s for {what}")
        select.select([], [], [], min(POLL_INTERVAL_SECONDS, _remaining(deadline)))


class PtyProcess:
    """A command on a fresh pty of its own, the way a user's terminal runs it."""

    def __init__(self, argv: list[str], env: dict[str, str] | None = None,
                 rows: int = 24, cols: int = 80) -> None:
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            try:
                import fcntl
                import struct
                import termios

                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                os.execvpe(argv[0], argv, env if env is not None else os.environ)
            finally:
                os._exit(127)

    def send(self, data: bytes) -> None:
        write_all(self.master, data)

    def expect(self, needle: bytes, timeout: float = STEP_TIMEOUT_SECONDS) -> bytes:
        return read_until(self.master, needle, timeout)

    def finish(self, timeout: float = STEP_TIMEOUT_SECONDS) -> Finished:
        """Reads to the end of the output and reaps the child."""
        output = read_until(self.master, None, timeout)
        status = wait_pid(self.pid, timeout)
        self.pid = -1
        os.close(self.master)
        self.master = -1
        return Finished(status=os.waitstatus_to_exitcode(status), output=output)

    def kill(self) -> None:
        if self.pid > 0:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
            self.pid = -1
        if self.master >= 0:
            os.close(self.master)
            self.master = -1


def run_on_pty(argv: list[str], timeout: float = STEP_TIMEOUT_SECONDS, **kwargs) -> Finished:
    """Runs `argv` on a pty to the end and returns its status and output."""
    process = PtyProcess(argv, **kwargs)
    try:
        return process.finish(timeout)
    finally:
        process.kill()
