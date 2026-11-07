"""test_m6_altscreen (design.md M6 criteria): a full-screen program in the
alt screen survives a cut that is recovered by a GridSnapshot. The snapshot
re-enters the alt screen and repaints it; when the program exits, the main
screen is back with what it held before.

The terminal under test is pyte, with the alt buffer that AltScreen adds.
"""

from __future__ import annotations

from e2e import PROMPT, Zet
from m6_support import (last_row, ALT_ENTER, ALT_LEAVE, SNAPSHOT_NOTICE, drain, link_cut,
                        render, require_pyte, rows_of, session_stream, typed,
                        wait_client_dropped, wait_recovered)

# The file vim shows, one distinct line each.
ALT_LINES = [f"alt-line-{n:02d}" for n in range(1, 6)]


def test_m6_altscreen(zet: Zet, clean_bashrc, iptables):
    require_pyte()
    terminal = zet.start(ZET_TEST_FORCE_PATH="B")
    try:
        # The first attach is a resume too, so B takes it: a snapshot.
        terminal.expect(SNAPSHOT_NOTICE)
        # What the main screen holds before vim; spelled so that the echo of
        # the command line does not match.
        terminal.send(typed('echo "MAIN""-MARK-1"; echo "MAIN""-MARK-2"'))
        terminal.expect(b"MAIN-MARK-2\r\n")
        terminal.expect(PROMPT)
        terminal.send(typed(
            "printf 'alt-line-%02d\\n' 1 2 3 4 5 > /tmp/zet-alt.txt; "
            "vim -u NONE -N -n /tmp/zet-alt.txt"))
        terminal.expect(ALT_ENTER)
        terminal.expect(b"alt-line-05")
        drain(terminal, 0.5)

        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
        after_cut = len(terminal.buffer)
        wait_recovered(terminal, after_cut)

        # The snapshot itself puts the terminal in the alt screen.
        assert session_stream(terminal, after_cut).count(ALT_ENTER) == 1

        screen = render(session_stream(terminal))
        assert screen.alt_active, rows_of(screen)
        rows = rows_of(screen)
        assert rows[:len(ALT_LINES)] == ALT_LINES, rows
        assert not any("MAIN-MARK" in row for row in rows), rows

        # vim still answers on the far side: leaving it brings the main screen
        # back, as it was.
        terminal.send(b":q\r")
        terminal.expect(ALT_LEAVE)
        terminal.expect(PROMPT)
        screen = render(session_stream(terminal))
        assert not screen.alt_active
        rows = rows_of(screen)
        assert "MAIN-MARK-1" in rows and "MAIN-MARK-2" in rows, rows
        assert not any("alt-line" in row for row in rows if "printf" not in row), rows
        assert last_row(rows).endswith("@zet-prompt$"), rows

        terminal.send(typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
