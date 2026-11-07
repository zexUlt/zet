"""What the M6 end-to-end tests share: cutting the link between zet and its
agent, a pyte screen that keeps the alternate buffer apart, and a remote
reflector that records the bytes the shell side received."""

from __future__ import annotations

import copy
import os
import subprocess
import time
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

import pytest

from e2e import (EXPECT_DEADLINE_SECONDS, LOST_NOTICE, PROMPT, REGAINED_NOTICE, TCP_ESTABLISHED, Terminal, Zet,
                 connection_to_agent, parent_of, socket_inodes, sockets_of)
from harness import STEP_TIMEOUT_SECONDS, Timeout, wait_gone, wait_until

try:
    import pyte  # type: ignore
    PYTE_AVAILABLE = True
except ImportError:
    PYTE_AVAILABLE = False

ROWS, COLS = 24, 80

# What a snapshot opens with (grid/snapshot.cpp): soft reset, main screen,
# erase.
SNAPSHOT_PREAMBLE = b"\x1b[!p\x1b[?1049l\x1b[2J"
ALT_ENTER = b"\x1b[?1049h"
ALT_LEAVE = b"\x1b[?1049l"

# What the client prints once a snapshot is applied (src/client/client.cpp).
SNAPSHOT_NOTICE = b"zet: screen restored, output during the cut skipped\r\n"
DIVERGED_NOTICE = b"zet: warning: the restored screen may differ from the remote one\r\n"


def require_pyte() -> None:
    if not PYTE_AVAILABLE and os.environ.get("ZET_BUILD_DIR"):
        pytest.fail("pyte is required when ZET_BUILD_DIR is set but was not found")
    if not PYTE_AVAILABLE:
        pytest.skip("pyte not installed")


def typed(command: str) -> bytes:
    return command.encode() + b"\r"


def drain(terminal: Terminal, quiet: float, deadline: float = EXPECT_DEADLINE_SECONDS) -> None:
    """Reads until the terminal has been silent for `quiet` seconds, within
    `deadline` in all: a session that never goes quiet is a failure, not a
    wait."""
    at = time.monotonic() + deadline
    while True:
        try:
            terminal.read_more(quiet)
        except Timeout:
            return
        if time.monotonic() >= at:
            raise Timeout(f"the terminal not quiet for {quiet}s within {deadline}s: "
                          f"{terminal.tail()!r}")


def shell_pid(terminal: Terminal) -> int:
    """The pid of the session's shell, from the shell itself."""
    # Spelled so that the echo of the command line does not match.
    terminal.send(typed('echo "she""ll=$$"'))
    terminal.expect(b"shell=")
    pid = int(terminal.expect(b"\r\n").strip())
    terminal.expect(PROMPT)
    return pid


def wait_recovered(terminal: Terminal, since: int, snapshot: bool = True) -> None:
    """Reads until zet says it is back (and that it applied a snapshot, unless
    `snapshot` is False; either notice may come first), lets the terminal go
    quiet, and moves the expect cursor past all of it."""
    needed = [REGAINED_NOTICE] + ([SNAPSHOT_NOTICE] if snapshot else [])
    terminal.read_until(lambda: all(n in terminal.buffer[since:] for n in needed),
                        "the reconnect and snapshot notices")
    drain(terminal, 0.5)
    terminal.consumed = len(terminal.buffer)


def _rules(port: int) -> list[list[str]]:
    return [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(port), "-j", "DROP"],
            ["INPUT", "-i", "lo", "-p", "tcp", "--sport", str(port), "-j", "DROP"]]


@contextmanager
def link_cut(zet: Zet, terminal: Terminal, iptables: str):
    """Drops every packet of the client's connection to its agent, both ways,
    so that it goes half-open instead of being reset; yields that socket and
    lifts the cut on the way out."""
    cut = connection_to_agent(terminal.pid, zet.host.port)
    rules = _rules(cut.remote_port)
    for rule in rules:
        subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
    try:
        yield cut
    finally:
        for rule in rules:
            subprocess.run([iptables, "-D", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)


def wait_client_dropped(terminal: Terminal, cut) -> None:
    """Waits for zet to give the cut connection up, reading the terminal
    meanwhile so that the client keeps counting the silence."""
    def dropped() -> bool:
        drain(terminal, 0)
        return cut.inode not in socket_inodes(terminal.pid)

    wait_until(dropped, "zet to drop the cut connection")


class AltScreen(pyte.HistoryScreen if PYTE_AVAILABLE else object):
    """pyte has one buffer and ignores ?1049; a terminal has two. This keeps
    the main screen and cursor aside while the alt screen is on, so that a
    test can tell which of them it is looking at."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.saved_main = None

    @property
    def alt_active(self) -> bool:
        return self.saved_main is not None

    def select_graphic_rendition(self, *attrs, **kwargs):
        # vim sends `CSI ? 4 m` (a private SGR); pyte raises on the flag, a
        # real terminal ignores the sequence.
        if not kwargs.get("private"):
            super().select_graphic_rendition(*attrs)

    def set_mode(self, *modes, **kwargs):
        if kwargs.get("private") and 1049 in modes:
            modes = tuple(m for m in modes if m != 1049)
            if self.saved_main is None:
                self.saved_main = (copy.deepcopy(self.buffer), self.cursor.x, self.cursor.y)
                self.erase_in_display(2)
                self.cursor_position()
        if modes:
            super().set_mode(*modes, **kwargs)

    def reset_mode(self, *modes, **kwargs):
        if kwargs.get("private") and 1049 in modes:
            modes = tuple(m for m in modes if m != 1049)
            if self.saved_main is not None:
                buffer, x, y = self.saved_main
                self.saved_main = None
                self.buffer = buffer
                self.dirty.update(range(self.lines))
                self.cursor.x, self.cursor.y = x, y
        if modes:
            super().reset_mode(*modes, **kwargs)


def render(stream: bytes, history: int = 2 * 1000) -> AltScreen:
    """What a terminal shows after `stream`."""
    screen = AltScreen(COLS, ROWS, history=history)
    pyte.ByteStream(screen).feed(stream)
    return screen


def session_stream(terminal: Terminal, start: int = 0) -> bytes:
    """What the terminal received from `start` on, without zet's own notices."""
    stream = bytes(terminal.buffer[start:])
    for notice in (LOST_NOTICE, REGAINED_NOTICE, SNAPSHOT_NOTICE, DIVERGED_NOTICE):
        stream = stream.replace(notice, b"")
    return stream


def rows_of(screen) -> list[str]:
    return [line.rstrip() for line in screen.display]


def last_row(rows: list[str]) -> str:
    """The last row that is not blank: the cursor's, when the screen is not
    full."""
    return next((row for row in reversed(rows) if row), "")


def history_of(screen) -> list[str]:
    return ["".join(line[x].data for x in range(COLS)).rstrip()
            for line in screen.history.top]


class Reflector:
    """A shell-side program that records what the shell's stdin received, raw,
    to a file the test reads: the terminal's echo proves nothing about the
    input stream, which is what the criteria are about."""

    def __init__(self) -> None:
        self.path = Path(f"/tmp/zet-reflect-{uuid.uuid4().hex}")

    def start(self, terminal: Terminal) -> None:
        # Raw, so that the line discipline neither edits nor echoes a key; the
        # marker is spelled so that the echo of this line does not match it.
        terminal.send(typed(f'stty raw -echo; echo "REFLE""CTING"; exec cat > {self.path}'))
        terminal.expect(b"REFLECTING")

    def received(self) -> bytes:
        try:
            return self.path.read_bytes()
        except FileNotFoundError:
            return b""

    def wait_for(self, want: bytes, what: str) -> None:
        """Waits until `want` or more has arrived, then requires exactly it:
        a lost key keeps it short, a duplicated one makes it differ."""
        wait_until(lambda: len(self.received()) >= len(want), what)
        assert self.received() == want

    def remove(self) -> None:
        self.path.unlink(missing_ok=True)


def key(index: int) -> bytes:
    """The index-th key of a stream in which no two are alike and none is a
    control character, zet's `~.` escape or a newline."""
    return f"<{index:04d}>".encode()


def keys(first: int, count: int) -> list[bytes]:
    return [key(i) for i in range(first, first + count)]


# The detached-output buffer the overflow tests shrink the agent's to, and a
# burst that is far more than it holds: `seq 1 20000` is about 109 KB.
DISCONNECT_BUFFER_MAX = 8192
BURST_LINES = 20_000


def burst_during_cut(zet: Zet, terminal: Terminal, iptables: str,
                     survives: bool = True) -> int:
    """Cuts the link, waits until both ends have dropped it, has the shell
    print a burst that overflows the agent's buffer while nobody is attached,
    and lifts the cut. With `survives` False the agent is expected to end the
    session on the overflow, and the burst is waited out by its exit.
    Returns the terminal buffer's length at the drop. The terminal must be at
    a prompt."""
    gate = Path(f"/tmp/zet-burst-gate-{uuid.uuid4().hex}")
    done = Path(f"/tmp/zet-burst-done-{uuid.uuid4().hex}")
    agent = parent_of(shell_pid(terminal))
    # The burst waits for the gate, which the test opens once both ends have
    # dropped the connection: written to a detached agent, it can only
    # overflow the buffer, never be acknowledged.
    terminal.send(typed(
        f'echo "MI""D"; while [ ! -e {gate} ]; do sleep 0.1; done; '
        f'seq 1 {BURST_LINES}; touch {done}; echo "EN""D"'))
    terminal.expect(b"MID\r\n")
    try:
        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
            wait_until(lambda: not any(s.state == TCP_ESTABLISHED
                                       for s in sockets_of(agent, zet.host.user)),
                       "the agent to detach")
            after_drop = len(terminal.buffer)
            gate.touch()
            if survives:
                wait_until(lambda: done.exists(), "the burst to be written")
            else:
                wait_gone(agent)
        return after_drop
    finally:
        gate.unlink(missing_ok=True)
        done.unlink(missing_ok=True)


# Short enough to run out within a test, long enough that the burst, which is
# written the moment both ends have dropped the link, is in well before it.
OVERFLOW_ORPHAN_TIMEOUT_S = 5

# What zet prints for SessionEnd kind=2 and exits with (src/client/client.cpp).
OVERFLOW_NOTICE = "zet: session output overflowed — reconnect to resume\r\n".encode()
OVERFLOW_STATUS = 70
# The agent's log line for a session it reaped on the overflow.
REAPED_ON_OVERFLOW = "ending the session reason=too much output while disconnected"


@dataclass
class OverflowOutcome:
    status: int
    shown: bytes  # what the terminal showed from the cut on
    agent_log: str


def overflow_without_snapshot(zet: Zet, iptables: str,
                              client_env: dict[str, str]) -> OverflowOutcome:
    """A session whose client cannot take a snapshot (`client_env` makes it so)
    is cut, and the shell prints far more than the agent's detached buffer
    holds. Returns how zet ended."""
    terminal = zet.start(client_env=client_env,
                         ZET_DISCONNECT_BUFFER_MAX=str(DISCONNECT_BUFFER_MAX),
                         ZET_ORPHAN_TIMEOUT=str(OVERFLOW_ORPHAN_TIMEOUT_S))
    try:
        terminal.expect(PROMPT)
        after_drop = burst_during_cut(zet, terminal, iptables, survives=False)
        status = terminal.finish()
        return OverflowOutcome(status=status, shown=bytes(terminal.buffer[after_drop:]),
                               agent_log=zet.agent_log.read_text(errors="replace"))
    finally:
        terminal.kill()
