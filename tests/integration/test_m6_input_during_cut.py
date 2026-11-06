"""test_m6_input_during_cut (design.md M6 criteria): the client types while
the link is down, and keeps typing through the reconnect and the delivery of
the snapshot (path B). No key is lost or doubled, and they arrive in order;
the input stream is judged by a Reflector on the shell side.
"""

from __future__ import annotations

import time

from e2e import EXPECT_DEADLINE_SECONDS, Zet
from harness import Timeout
from m6_support import (Reflector, SNAPSHOT_NOTICE, REGAINED_NOTICE, key, keys,
                        link_cut, shell_pid, wait_client_dropped)

DURING_CUT = 20


def test_m6_input_during_cut(zet: Zet, clean_bashrc, iptables):
    reflector = Reflector()
    terminal = zet.start(ZET_TEST_FORCE_PATH="B")
    try:
        terminal.expect(SNAPSHOT_NOTICE)
        shell_pid(terminal)
        reflector.start(terminal)

        sent = 0
        after_cut = len(terminal.buffer)
        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
            # Typed into a client that knows it is disconnected.
            for item in keys(sent, DURING_CUT):
                terminal.send(item)
            sent += DURING_CUT

        # The cut is lifted: type one key per turn through the reconnect and
        # until the snapshot has been written to the terminal, so that keys
        # are in flight while it is being delivered.
        def recovered() -> bool:
            seen = terminal.buffer[after_cut:]
            return REGAINED_NOTICE in seen and SNAPSHOT_NOTICE in seen

        deadline = time.monotonic() + EXPECT_DEADLINE_SECONDS
        while not recovered():
            if time.monotonic() >= deadline:
                raise Timeout(f"no reconnect and snapshot within {EXPECT_DEADLINE_SECONDS}s: "
                              f"{terminal.tail()!r}")
            terminal.send(key(sent))
            sent += 1
            try:
                terminal.read_more(0.05)
            except Timeout:
                pass
        assert sent > DURING_CUT

        terminal.send(key(sent))
        sent += 1
        reflector.wait_for(b"".join(keys(0, sent)), "every key to arrive, once")
    finally:
        terminal.kill()
        reflector.remove()
