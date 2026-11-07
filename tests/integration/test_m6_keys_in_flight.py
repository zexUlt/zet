"""test_m6_keys_in_flight (design.md M6 criteria): the client types at the
moment the link is cut, before either end has noticed. Those keys went into a
connection that delivers nothing; after the reconnect (path B) the shell side
has every one of them, once and in order.

The shell side is a Reflector: `cat` on a raw tty, writing what it reads to a
file, so the terminal's echo is no part of the evidence.
"""

from __future__ import annotations

from e2e import Zet
from m6_support import (Reflector, SNAPSHOT_NOTICE, key, keys, link_cut, shell_pid,
                        wait_client_dropped, wait_recovered)

BEFORE = 10
IN_FLIGHT = 40


def test_m6_keys_in_flight(zet: Zet, clean_bashrc, iptables):
    reflector = Reflector()
    terminal = zet.start(ZET_TEST_FORCE_PATH="B")
    try:
        terminal.expect(SNAPSHOT_NOTICE)
        shell_pid(terminal)
        reflector.start(terminal)

        # Keys that were acknowledged before the cut are not sent again.
        for item in keys(0, BEFORE):
            terminal.send(item)
        reflector.wait_for(b"".join(keys(0, BEFORE)), "the first keys to arrive")

        after_cut = len(terminal.buffer)
        with link_cut(zet, terminal, iptables) as cut:
            # No wait between the cut and the keys: the client has not noticed
            # it and writes them to a connection that is already dead.
            for item in keys(BEFORE, IN_FLIGHT):
                terminal.send(item)
            wait_client_dropped(terminal, cut)
        wait_recovered(terminal, after_cut)

        # One more key, to see where the stream ends: a duplicate would have
        # pushed it further out.
        terminal.send(key(BEFORE + IN_FLIGHT))
        reflector.wait_for(b"".join(keys(0, BEFORE + IN_FLIGHT + 1)),
                           "every key to arrive, once")
    finally:
        terminal.kill()
        reflector.remove()
