"""test_m6_keys_detection_window (design.md M6 criteria): the path is cut
half-open, so that neither end notices for the whole silence timeout, and
the client types all through that window. After the reconnect the shell side
has every key, once and in order (judged by a Reflector).

The design's window is 15 s; the suite shortens the silence timeout, and here
lengthens it to 8 s so that the keys are typed well inside it (about 6 s of margin).
"""

from __future__ import annotations

from e2e import PROMPT, TCP_ESTABLISHED, Zet, sockets_of
from harness import wait_until
from m6_support import (Reflector, SNAPSHOT_NOTICE, drain, key, keys, link_cut,
                        parent_of, shell_pid, wait_client_dropped, wait_recovered)

SILENCE_MS = "8000"
BATCH = 15


def test_m6_keys_detection_window(zet: Zet, clean_bashrc, iptables):
    reflector = Reflector()
    terminal = zet.start(client_env={"ZET_PEER_SILENCE_TIMEOUT": SILENCE_MS},
                         ZET_PEER_SILENCE_TIMEOUT=SILENCE_MS)
    try:
        terminal.expect(PROMPT)
        agent = parent_of(shell_pid(terminal))
        reflector.start(terminal)

        def agent_attached() -> bool:
            return any(s.state == TCP_ESTABLISHED
                       for s in sockets_of(agent, zet.host.user))

        sent = 0
        after_cut = len(terminal.buffer)
        with link_cut(zet, terminal, iptables) as cut:
            # Two batches, a second apart (the terminal is read meanwhile, so
            # the client keeps counting): both end inside the window, with the
            # agent still holding the connection.
            for _ in range(2):
                for item in keys(sent, BATCH):
                    terminal.send(item)
                sent += BATCH
                drain(terminal, 1.0)
                assert cut.inode in {s.inode for s in sockets_of(terminal.pid)}
                assert agent_attached()
            wait_client_dropped(terminal, cut)
            wait_until(lambda: not agent_attached(), "the agent to detach")
        wait_recovered(terminal, after_cut, snapshot=False)

        terminal.send(key(sent))
        sent += 1
        reflector.wait_for(b"".join(keys(0, sent)), "every key to arrive, once")
    finally:
        terminal.kill()
        reflector.remove()
