"""The agent's read gate against its silence timer (design.md §9), through
the real zet-agent: while the shell does not read its input, the agent stops
reading the socket, and the client's silence in return must not be taken for
a dead path."""

from __future__ import annotations

import hashlib
import random
import string

from e2e import LOST_NOTICE, PEER_SILENCE_TIMEOUT_MS, PROMPT, Zet
from harness import STEP_TIMEOUT_SECONDS

# More than the agent's pty input queue and the pty's own buffer together, so
# that the agent's read gate closes, and less than the client's resend buffer
# (4 MiB), so that the client can hand all of it over without waiting.
INPUT_BYTES = 3 * 1024 * 1024

# The shell reads nothing for this long: more than three silence timeouts of
# the connection, which is what the agent would have given up after.
STALL_S = 4 * PEER_SILENCE_TIMEOUT_MS // 1000


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


def test_a_shell_that_does_not_read_does_not_cost_the_link(zet: Zet, clean_bashrc):
    # Letters only: nothing in it is an escape (`~.`) or a control character.
    rng = random.Random(41)
    data = "".join(rng.choices(string.ascii_lowercase, k=INPUT_BYTES)).encode()
    digest = hashlib.md5(data).hexdigest()  # noqa: S324 - a checksum, not a secret
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        # Raw and silent first, so that nothing is echoed or cut to lines;
        # the marker says the mode is set before any input is sent. Spelled
        # so that the echo of the command line does not match.
        terminal.send(_typed(
            f'stty raw -echo; echo "rea""dy"; sleep {STALL_S}; '
            f"head -c {INPUT_BYTES} | md5sum"))
        terminal.expect(b"ready")
        terminal.send(data)
        # The sum is what the shell read once it woke: all of it, in order.
        terminal.expect(digest.encode(), timeout=STALL_S + STEP_TIMEOUT_SECONDS)
        assert LOST_NOTICE not in terminal.buffer
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
        assert LOST_NOTICE not in terminal.buffer
    finally:
        terminal.kill()
