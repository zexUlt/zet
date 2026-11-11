"""What the M7 predictive-echo tests share: the client's overlay log, a pump
for a terminal that is slow on purpose, and a pyte view of what it shows.

The client runs with --predict=<mode> and ZET_TEST_OVERLAY_LOG (design.md
§16, "M7 — тестовые критерии"): the log has one "offset length" line for every
piece of overlay the client put into its stdout, offsets counted in stdout
bytes. Taking those pieces out of the bytes the terminal received leaves the
agent's stream; rendering both shows where a prediction was left behind.
"""

from __future__ import annotations

import time
import uuid
from pathlib import Path

from e2e import PROMPT, Terminal, Zet
from harness import PtyProcess, Timeout
from m6_support import drain, render, require_pyte, rows_of, typed


class Predicted:
    """A zet session with predictive echo, its overlay log and a pump."""

    def __init__(self, zet: Zet, mode: str, tmp_path: Path, client_env: dict[str, str] | None = None,
                 mirror_dump: bool = False, **agent_env: str) -> None:
        self.zet = zet
        self.mode = mode
        self.log = tmp_path / f"overlay-{uuid.uuid4().hex}.log"
        self.dump = tmp_path / f"mirror-{uuid.uuid4().hex}.bin"
        argv = zet.argv(zet.agent_env(**agent_env))
        argv[argv.index("--predict=never")] = f"--predict={mode}"
        env = zet.env()
        env["ZET_TEST_OVERLAY_LOG"] = str(self.log)
        if mirror_dump:
            env["ZET_TEST_MIRROR_DUMP"] = str(self.dump)
        env.update(client_env or {})
        self.terminal = Terminal(PtyProcess(argv, env=env))

    # -- the overlay log

    def entries(self) -> list[tuple[int, int]]:
        try:
            lines = self.log.read_text().splitlines()
        except FileNotFoundError:
            return []
        return [(int(a), int(b)) for a, b in (line.split() for line in lines)]

    def stream(self) -> bytes:
        """Everything the terminal received. The notices zet prints stay in:
        with predictions on, they go through its stdout queue and the log's
        offsets count them."""
        return bytes(self.terminal.buffer)

    def agent_stream(self) -> bytes:
        """The terminal's bytes with every overlay piece taken out."""
        return strip_overlay(self.stream(), self.entries())

    # -- driving

    def pump(self, seconds: float) -> None:
        """Reads the terminal for `seconds`, whatever comes."""
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return
            try:
                self.terminal.read_more(left)
            except Timeout:
                return

    def settle(self, quiet: float = 0.7) -> None:
        """Until the terminal has been silent for `quiet` seconds."""
        drain(self.terminal, quiet)

    def prime(self) -> None:
        """Waits for the prompt on a cleared screen, so that the engine may
        predict (a column it does not trust gets no predictions, §9)."""
        t = self.terminal
        t.expect(PROMPT)
        t.send(typed("clear"))
        t.expect(PROMPT)
        self.settle()
        t.consumed = len(t.buffer)

    def kill(self) -> None:
        self.terminal.kill()


def strip_overlay(stream: bytes, entries: list[tuple[int, int]]) -> bytes:
    out = bytearray()
    position = 0
    for offset, length in entries:
        assert offset >= position, (entries, position)
        out += stream[position:offset]
        position = offset + length
    out += stream[position:]
    return bytes(out)


def cursor_of(screen) -> tuple[int, int]:
    return screen.cursor.x, screen.cursor.y


def assert_no_stray_overlay(predicted: Predicted) -> None:
    """What the terminal shows equals what it would show without any overlay:
    every cell and the cursor. Every piece was taken off again."""
    require_pyte()
    shown = render(predicted.stream())
    agents = render(predicted.agent_stream())
    assert rows_of(shown) == rows_of(agents), (rows_of(shown), rows_of(agents))
    assert cursor_of(shown) == cursor_of(agents)
    for y in range(shown.lines):
        for x in range(shown.columns):
            a, b = shown.buffer[y][x], agents.buffer[y][x]
            assert (a.data, a.underscore) == (b.data, b.underscore), (y, x, a, b)
