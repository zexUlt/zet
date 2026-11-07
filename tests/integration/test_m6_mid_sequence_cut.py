"""test_m6_mid_sequence_cut (design.md M6 criteria, §9 path B): the shell's
output stops inside a DCS when the link is cut. The agent's parser cannot
reach Ground within GROUND_WAIT_TIMEOUT, so it is reset, the snapshot goes out
flagged PossiblyDiverged, and the client says so on stderr, after the
"screen restored" notice and once. The session works after it.
"""

from __future__ import annotations

from e2e import PROMPT, REGAINED_NOTICE, Zet
from m6_support import (DIVERGED_NOTICE, SNAPSHOT_NOTICE, drain, link_cut, typed,
                        wait_client_dropped)


def test_m6_mid_sequence_cut(zet: Zet, clean_bashrc, iptables):
    terminal = zet.start(ZET_TEST_FORCE_PATH="B")
    try:
        # The first attach is a resume too, so B takes it: a snapshot, which
        # is not flagged, the parser being in Ground.
        terminal.expect(SNAPSHOT_NOTICE)
        drain(terminal, 0.5)
        assert DIVERGED_NOTICE not in terminal.buffer

        # A sixel DCS that never gets its ST, and then the shell goes quiet.
        # (Were it to print its prompt, the prompt's own escape sequences
        # would end the string.)
        terminal.send(typed("printf '\\033P0;0;0q#0!8'; sleep 120"))
        drain(terminal, 1.0)

        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
        after_cut = len(terminal.buffer)

        # The agent waits GROUND_WAIT_TIMEOUT (5 s) for a Ground that does not
        # come, then builds anyway.
        terminal.read_until(
            lambda: all(n in terminal.buffer[after_cut:]
                        for n in (REGAINED_NOTICE, SNAPSHOT_NOTICE, DIVERGED_NOTICE)),
            "the reconnect, snapshot and divergence notices")
        drain(terminal, 0.5)
        terminal.consumed = len(terminal.buffer)
        received = bytes(terminal.buffer[after_cut:])

        assert received.count(SNAPSHOT_NOTICE) == 1, received[-400:]
        assert received.count(DIVERGED_NOTICE) == 1, received[-400:]
        assert received.index(SNAPSHOT_NOTICE) < received.index(DIVERGED_NOTICE), received[-400:]

        # The parser is in Ground again: what the shell prints once the sleep
        # is interrupted is read as text.
        terminal.send(b"\x03")
        terminal.expect(PROMPT)
        terminal.send(typed('echo "af""ter=$((6*7))"'))
        terminal.expect(b"after=42")
        terminal.send(typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
