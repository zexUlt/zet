"""test_m6_v1_compat (design.md M6 criteria): a client capped at protocol v1
(ZET_TEST_VERSION_CEILING=1) against an agent that speaks v2 negotiates v1.
A cut that ZET_TEST_FORCE_PATH=B would turn into a snapshot is recovered by
byte replay instead, and an overflow reaps the session quietly: no snapshot
and no SessionEnd(kind=2), which v1 does not have.
"""

from __future__ import annotations

from e2e import GAVE_UP_STATUS, PROMPT, REGAINED_NOTICE, Zet
from m6_support import (OVERFLOW_NOTICE, REAPED_ON_OVERFLOW, SNAPSHOT_NOTICE,
                        SNAPSHOT_PREAMBLE, link_cut, overflow_without_snapshot,
                        session_stream, typed, wait_client_dropped)

V1 = {"ZET_TEST_VERSION_CEILING": "1"}


def test_m6_v1_compat(zet: Zet, clean_bashrc, iptables):
    # The short cut: B is forced on the agent, and v1 cannot take it.
    terminal = zet.start(client_env=V1, ZET_TEST_FORCE_PATH="B")
    try:
        terminal.expect(PROMPT)
        terminal.send(typed('echo "be""fore=$((6*7))"'))
        terminal.expect(b"before=42")
        terminal.expect(PROMPT)
        after_cut = len(terminal.buffer)
        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
        terminal.expect(REGAINED_NOTICE)
        terminal.send(typed('echo "af""ter=$((6*7))"'))
        terminal.expect(b"after=42")
        terminal.expect(PROMPT)
        shown = bytes(terminal.buffer[after_cut:])
        assert SNAPSHOT_PREAMBLE not in shown and SNAPSHOT_NOTICE not in shown, shown
        terminal.send(typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()

    # The overflow: reaped, and the client is not told why.
    outcome = overflow_without_snapshot(zet, iptables, V1)
    assert REAPED_ON_OVERFLOW in outcome.agent_log, outcome.agent_log
    assert SNAPSHOT_PREAMBLE not in outcome.shown
    assert SNAPSHOT_NOTICE not in outcome.shown
    assert OVERFLOW_NOTICE not in outcome.shown
    assert outcome.status == GAVE_UP_STATUS, outcome
