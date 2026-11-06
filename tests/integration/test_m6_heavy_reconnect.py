"""test_m6_heavy_reconnect (design.md §9 path B, M6 criteria): a session that
prints far more than the disconnect buffer holds while the client is gone
comes back as exactly one GridSnapshot, within SNAPSHOT_MAX_BYTES, whose
rendering is the screen the shell is showing, and keeps working.

ZET_DISCONNECT_BUFFER_MAX shrinks the buffer to a few KiB so that a ~110 KB
burst overflows it, instead of the 100 MB the 16 MiB default would take.
"""

from __future__ import annotations

import os
import subprocess
import uuid
from pathlib import Path

import pytest

from e2e import (LOST_NOTICE, PROMPT, REGAINED_NOTICE, TCP_ESTABLISHED, Terminal, Zet,
                 connection_to_agent, parent_of, sockets_of)
from harness import STEP_TIMEOUT_SECONDS, Timeout, wait_until

try:
    import pyte  # type: ignore
    PYTE_AVAILABLE = True
except ImportError:
    PYTE_AVAILABLE = False

# wire/limits.hpp SNAPSHOT_MAX_BYTES.
SNAPSHOT_MAX_BYTES = 4 * 1024 * 1024

DISCONNECT_BUFFER_MAX = 8192
# `seq 1 BURST_LINES` is about 109 KB: more than ten times the buffer.
BURST_LINES = 20_000
ROWS, COLS = 24, 80
# grid/limits.hpp DEFAULT_SCROLLBACK_LIMIT; the agent runs with the default.
SCROLLBACK_LIMIT = 1000

# What a snapshot opens with (grid/snapshot.cpp), as in test_m6_paths.py.
SNAPSHOT_PREAMBLE = b"\x1b[!p\x1b[?1049l\x1b[2J"
# What the client prints once a snapshot is applied (src/client/client.cpp).
SNAPSHOT_NOTICE = b"zet: screen restored, output during the cut skipped\r\n"
DIVERGED_NOTICE = b"zet: warning: the restored screen may differ from the remote one\r\n"


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


def _drain(terminal: Terminal, quiet: float) -> None:
    """Reads until the terminal has been silent for `quiet` seconds."""
    while True:
        try:
            terminal.read_more(quiet)
        except Timeout:
            return


def test_m6_heavy_reconnect(zet: Zet, clean_bashrc, iptables):
    if not PYTE_AVAILABLE and os.environ.get("ZET_BUILD_DIR"):
        pytest.fail("pyte is required when ZET_BUILD_DIR is set but was not found")
    if not PYTE_AVAILABLE:
        pytest.skip("pyte not installed")

    gate = Path(f"/tmp/zet-heavy-gate-{uuid.uuid4().hex}")
    done = Path(f"/tmp/zet-heavy-done-{uuid.uuid4().hex}")
    terminal = zet.start(ZET_DISCONNECT_BUFFER_MAX=str(DISCONNECT_BUFFER_MAX))
    try:
        terminal.expect(PROMPT)
        agent = parent_of(_shell_pid(terminal))
        # The burst waits for the gate, which the test opens once both ends
        # have dropped the connection: it is written to a detached agent, so
        # it can only overflow the buffer, never be acknowledged.
        terminal.send(_typed(
            f'echo "MI""D"; while [ ! -e {gate} ]; do sleep 0.1; done; '
            f'seq 1 {BURST_LINES}; touch {done}; echo "EN""D"'))
        terminal.expect(b"MID\r\n")

        cut = connection_to_agent(terminal.pid, zet.host.port)
        rules = [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(cut.remote_port), "-j", "DROP"],
                 ["INPUT", "-i", "lo", "-p", "tcp", "--sport", str(cut.remote_port), "-j", "DROP"]]
        for rule in rules:
            subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        try:
            # Both ends find the silence on their own timers. The terminal is
            # read meanwhile so that the client keeps counting it.
            def dropped() -> bool:
                _drain(terminal, 0)
                return cut.inode not in {s.inode for s in sockets_of(terminal.pid)}

            wait_until(dropped, "zet to drop the cut connection")
            wait_until(lambda: not any(s.state == TCP_ESTABLISHED
                                       for s in sockets_of(agent, zet.host.user)),
                       "the agent to detach")
            after_drop = len(terminal.buffer)
            gate.touch()
            wait_until(lambda: done.exists(), "the burst to be written")
        finally:
            for rule in rules:
                subprocess.run([iptables, "-D", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)

        # Either notice may come first.
        terminal.read_until(
            lambda: SNAPSHOT_NOTICE in terminal.buffer[after_drop:]
            and REGAINED_NOTICE in terminal.buffer[after_drop:],
            "the snapshot and the reconnect notices")
        _drain(terminal, 1.0)
        received = bytes(terminal.buffer[after_drop:])

        assert received.count(SNAPSHOT_PREAMBLE) == 1, received[-400:]
        assert received.count(SNAPSHOT_NOTICE) == 1
        assert len(received) <= SNAPSHOT_MAX_BYTES, len(received)

        # The client's own words are not the session's.
        stream = received
        for notice in (LOST_NOTICE, REGAINED_NOTICE, SNAPSHOT_NOTICE, DIVERGED_NOTICE):
            stream = stream.replace(notice, b"")
        # HistoryScreen keeps the lines that scrolled off the top, which is
        # where a snapshot's scrollback section lands.
        screen = pyte.HistoryScreen(COLS, ROWS, history=2 * SCROLLBACK_LIMIT)
        pyte.ByteStream(screen).feed(stream)
        rows = [line.rstrip() for line in screen.display]

        # The shell shows the tail of the burst, END and its prompt; none of
        # that was ever in the replay buffer.
        assert rows[-1].endswith("@zet-prompt$"), rows
        assert rows[-2] == "END", rows
        want = [str(n) for n in range(BURST_LINES - (ROWS - 3), BURST_LINES + 1)]
        assert rows[:ROWS - 2] == want, rows
        assert screen.cursor.y == ROWS - 1, screen.cursor.y

        # The snapshot also carries the agent's scrollback: the last
        # SCROLLBACK_LIMIT lines above the visible ones (1000 short lines are
        # a few KiB, so no step of the degrade ladder trims them), oldest
        # first, and nothing else.
        history = ["".join(line[x].data for x in range(COLS)).rstrip()
                   for line in screen.history.top]
        first_visible = BURST_LINES - (ROWS - 3)
        want_history = [str(n) for n in range(first_visible - SCROLLBACK_LIMIT, first_visible)]
        assert history == want_history, (len(history), history[:3], history[-3:])

        # The session works after the snapshot.
        terminal.send(_typed('echo "mar""ker=$((6*7))"'))
        terminal.expect(b"marker=42")
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
        gate.unlink(missing_ok=True)
        done.unlink(missing_ok=True)


def _shell_pid(terminal: Terminal) -> int:
    """The pid of the session's shell, from the shell itself."""
    # Spelled so that the echo of the command line does not match.
    terminal.send(_typed('echo "she""ll=$$"'))
    terminal.expect(b"shell=")
    pid = int(terminal.expect(b"\r\n").strip())
    terminal.expect(PROMPT)
    return pid
