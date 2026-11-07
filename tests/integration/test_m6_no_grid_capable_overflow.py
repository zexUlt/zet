"""test_m6_no_grid_capable_overflow (design.md M6 criteria): at ver=2 with a
client that did not advertise GRID_CAPABLE, an overflow of the detached-output
buffer ends the session; it never becomes a snapshot.

ZET_TEST_NO_GRID_CAPABLE=1 leaves the TLV out of the client's Hello and
ZET_DISCONNECT_BUFFER_MAX shrinks the agent's buffer so that a short burst
overflows it.

By design (§7, §9) this is a quiet reap, the same as at ver=1. The overflow
only happens while detached, so there is no connection to send
SessionEnd(kind=2) to: the agent kills the session and exits, and the client
finds the port closed until it gives up (status 255). Kind 2 is reserved.
"""

from __future__ import annotations

from m6_support import (REAPED_ON_OVERFLOW, SNAPSHOT_NOTICE, SNAPSHOT_PREAMBLE,
                        overflow_without_snapshot)
from e2e import GAVE_UP_STATUS, Zet

NO_GRID_CAPABLE = {"ZET_TEST_NO_GRID_CAPABLE": "1"}


def test_m6_no_grid_capable_overflow(zet: Zet, clean_bashrc, iptables):
    outcome = overflow_without_snapshot(zet, iptables, NO_GRID_CAPABLE)
    assert REAPED_ON_OVERFLOW in outcome.agent_log, outcome.agent_log
    assert SNAPSHOT_PREAMBLE not in outcome.shown
    assert SNAPSHOT_NOTICE not in outcome.shown
    assert outcome.status == GAVE_UP_STATUS, outcome
