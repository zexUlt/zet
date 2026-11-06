"""test_m6_no_grid_capable_overflow (design.md M6 criteria): at ver=2 with a
client that did not advertise GRID_CAPABLE, an overflow of the detached-output
buffer ends the session; it never becomes a snapshot.

ZET_TEST_NO_GRID_CAPABLE=1 leaves the TLV out of the client's Hello and
ZET_DISCONNECT_BUFFER_MAX shrinks the agent's buffer so that a short burst
overflows it.

The design (§7, §9) has the client told so, by SessionEnd(kind=2): a notice
and exit status 70. The binaries do not do that. The agent reaps the session
on the overflow, and a reaped agent leaves its loop and closes the listener at
once, so that there is nobody left to send the SessionEnd to; the client
finds the port closed until it gives up. The first test holds what is true
today; the second is the criterion as written, expected to fail until the
agent lingers long enough to deliver it.
"""

from __future__ import annotations

import pytest

from m6_support import (OVERFLOW_NOTICE, OVERFLOW_STATUS, REAPED_ON_OVERFLOW,
                        SNAPSHOT_NOTICE, SNAPSHOT_PREAMBLE, overflow_without_snapshot)
from e2e import GAVE_UP_STATUS, Zet

NO_GRID_CAPABLE = {"ZET_TEST_NO_GRID_CAPABLE": "1"}


def test_m6_no_grid_capable_overflow(zet: Zet, clean_bashrc, iptables):
    outcome = overflow_without_snapshot(zet, iptables, NO_GRID_CAPABLE)
    assert REAPED_ON_OVERFLOW in outcome.agent_log, outcome.agent_log
    assert SNAPSHOT_PREAMBLE not in outcome.shown
    assert SNAPSHOT_NOTICE not in outcome.shown
    assert outcome.status == GAVE_UP_STATUS, outcome


@pytest.mark.xfail(strict=True, reason="the reaped agent exits before it can send "
                                       "SessionEnd(kind=2): design.md §9 is not met")
def test_m6_no_grid_capable_overflow_reported(zet: Zet, clean_bashrc, iptables):
    outcome = overflow_without_snapshot(zet, iptables, NO_GRID_CAPABLE)
    assert OVERFLOW_NOTICE in outcome.shown, outcome.shown[-300:]
    assert outcome.status == OVERFLOW_STATUS
