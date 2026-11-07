"""test_m6_scrollback_survives (design.md M6 criteria): a session that has
printed more lines than the agent's scrollback holds comes back from a cut as
a snapshot whose history is the last SCROLLBACK_LIMIT lines that scrolled off
the screen, and nothing older.

ZET_TEST_SCROLLBACK_LIMIT sets the agent's scrollback: 0 is the design's
`--scrollback 0` boundary case (no history, no crash), a small number shows
that the limit is the agent's and not a constant of the snapshot.
"""

from __future__ import annotations

import pytest

from e2e import PROMPT, Zet
from m6_support import (ROWS, SNAPSHOT_NOTICE, history_of, last_row, link_cut,
                        render, require_pyte, rows_of, session_stream, typed,
                        wait_client_dropped, wait_recovered)

# grid/limits.hpp DEFAULT_SCROLLBACK_LIMIT; the agent runs with it unless told.
SCROLLBACK_LIMIT = 1000
# More than the limit and the screen together.
LINES = 2500


def _line(n: int) -> str:
    return f"line-{n:05d}"


@pytest.mark.parametrize("limit", [SCROLLBACK_LIMIT, 50, 0])
def test_m6_scrollback_survives(zet: Zet, clean_bashrc, iptables, limit):
    require_pyte()
    extra = {} if limit == SCROLLBACK_LIMIT else {"ZET_TEST_SCROLLBACK_LIMIT": str(limit)}
    terminal = zet.start(ZET_TEST_FORCE_PATH="B", **extra)
    try:
        terminal.expect(SNAPSHOT_NOTICE)
        terminal.send(typed(f"for ((i=1; i<={LINES}; i++)); do printf 'line-%05d\\n' $i; done; "
        'echo "EN""D"'))
        terminal.expect(b"END\r\n")
        terminal.expect(PROMPT)

        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
        after_cut = len(terminal.buffer)
        wait_recovered(terminal, after_cut)

        # Only the snapshot: the live stream before the cut put every line
        # through this terminal's own scrollback.
        screen = render(session_stream(terminal, after_cut))
        rows = rows_of(screen)
        assert last_row(rows).endswith("@zet-prompt$"), rows
        assert rows[ROWS - 2] == "END", rows
        visible = rows[:ROWS - 2]
        first = LINES - len(visible) + 1
        assert visible == [_line(n) for n in range(first, LINES + 1)], rows

        history = history_of(screen)
        want = [_line(n) for n in range(max(1, first - limit), first)]
        assert len(want) == limit
        assert history == want, (len(history), history[:2], history[-2:])

        terminal.send(typed('echo "af""ter=$((6*7))"'))
        terminal.expect(b"after=42")
        terminal.expect(PROMPT)
        terminal.send(typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
