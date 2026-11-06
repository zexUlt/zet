"""The agent keeps reading its socket while the pty will not take input.

A foreground program that writes a lot of output before it reads its input
(`seq` before `head`) leaves the client's keystrokes waiting in the agent.
The client's Acks for that output travel in the same ordered stream behind
those keystrokes: an agent that stops reading the socket until the pty has
taken the input never sees them, its output window (RESEND_BUFFER_MAX) fills,
it stops reading the pty, and the program never gets to its input (design.md
§9). The session must instead deliver all of the output and all of the input.
"""

from __future__ import annotations

import hashlib
import os
import random
import select
import time

import pytest

from e2e import LOST_NOTICE, PROMPT, Terminal, Zet
from harness import STEP_TIMEOUT_SECONDS

# More than RESEND_BUFFER_MAX (4 MiB) of output ahead of the input is read.
SEQ_LINES = 1_500_000

# The agent's queue for input the pty has not taken is
# max(RESEND_BUFFER_MAX, DISCONNECT_BUFFER_MAX) = 16 MiB (zet/session/limits.hpp,
# design.md §9). While the shell does not read, only the acknowledgement on
# delivery keeps the client from sending past it: an agent that acknowledged on
# receipt would take everything the client has and overflow the queue. The
# input is 4 MiB past it, so that this test fails for that agent.
QUEUE_MAX = 16 * 1024 * 1024
INPUT_SIZE = QUEUE_MAX + 4 * 1024 * 1024

# One bound for the whole exchange, so that a deadlock fails the test instead
# of hanging it. A passing run takes a fraction of it.
OVERALL_DEADLINE_SECONDS = 120.0

# The end of md5sum's line; the prompt may follow it.
DIGEST_END = b"  -\n"

WRITE_CHUNK = 16 * 1024
READ_CHUNK = 256 * 1024


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


def _keystrokes() -> bytes:
    # Letters only: no escape character of the client (`~`), no line ending,
    # nothing either terminal would interpret.
    rng = random.Random(1234)
    return bytes(rng.choices(b"abcdefghijklmnopqrstuvwxyz", k=INPUT_SIZE))


def test_input_the_pty_cannot_take_yet_does_not_stall_the_output(zet: Zet, clean_bashrc):
    keystrokes = _keystrokes()
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        # "ready" is spelled so that the echo of the command line does not
        # match; stty raw turns the echo off before the program's output.
        terminal.send(_typed(
            f'stty raw -echo; echo "rea""dy"; seq 1 {SEQ_LINES}; '
            f"head -c {INPUT_SIZE} | md5sum"))
        terminal.expect(b"ready\n")

        received = _exchange(terminal, keystrokes)
        # End the shell, so that the agent goes with it rather than lingering
        # until its orphan timeout with the installed binary still open.
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()

    assert LOST_NOTICE not in received
    expected_seq = b"".join(b"%d\n" % n for n in range(1, SEQ_LINES + 1))
    digest = hashlib.md5(keystrokes).hexdigest().encode()
    assert received.startswith(expected_seq + digest + b"  -\n")


def _exchange(terminal: Terminal, keystrokes: bytes) -> bytes:
    """Sends `keystrokes` while reading what the session prints, up to the
    line md5sum ends it with. Fails, rather than waits, once a step or the
    whole exchange takes too long or the client reports a lost connection."""
    master = terminal.process.master
    # A blocking write would stop this loop from reading while the terminal
    # is full, which is a deadlock of the test's own making.
    os.set_blocking(master, False)
    # What was read past "ready" by the terminal already.
    received = bytearray(terminal.buffer[terminal.consumed:])
    terminal.consumed = len(terminal.buffer)
    sent = 0
    started = time.monotonic()
    last_progress = started
    done = False
    while not done:
        now = time.monotonic()
        if now - started > OVERALL_DEADLINE_SECONDS:
            pytest.fail(f"the exchange took over {OVERALL_DEADLINE_SECONDS}s: "
                        f"sent {sent} of {len(keystrokes)}, read {len(received)}; "
                        f"tail {bytes(received[-200:])!r}")
        if now - last_progress > STEP_TIMEOUT_SECONDS:
            pytest.fail(f"no progress for {STEP_TIMEOUT_SECONDS}s: sent {sent} of "
                        f"{len(keystrokes)}, read {len(received)}; "
                        f"tail {bytes(received[-200:])!r}")
        writing = [master] if sent < len(keystrokes) else []
        readable, writable, _ = select.select([master], writing, [], 1.0)
        if readable:
            try:
                chunk = os.read(master, READ_CHUNK)
            except BlockingIOError:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                pytest.fail(f"the terminal ended: {bytes(received[-200:])!r}")
            received += chunk
            done = DIGEST_END in received[-len(chunk) - len(DIGEST_END):]
            last_progress = time.monotonic()
            if LOST_NOTICE in received[-len(chunk) - len(LOST_NOTICE):]:
                pytest.fail("the client lost its connection: "
                            f"sent {sent} of {len(keystrokes)}, read {len(received)}")
        if writable:
            try:
                sent += os.write(master, keystrokes[sent:sent + WRITE_CHUNK])
                last_progress = time.monotonic()
            except BlockingIOError:
                pass
    return bytes(received)
