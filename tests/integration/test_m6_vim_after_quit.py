"""test_m6_vim_after_quit (design.md M6 criteria): vim has been in the alt
screen and left it before the cut. The snapshot that recovers the session
does not enter the alt screen, and the screen is the ordinary one.
"""

from __future__ import annotations

from e2e import PROMPT, Zet
from m6_support import (last_row, ALT_ENTER, ALT_LEAVE, SNAPSHOT_NOTICE, drain, link_cut,
                        render, require_pyte, rows_of, session_stream, typed,
                        wait_client_dropped, wait_recovered)


def test_m6_vim_after_quit(zet: Zet, clean_bashrc, iptables):
    require_pyte()
    terminal = zet.start(ZET_TEST_FORCE_PATH="B")
    try:
        terminal.expect(SNAPSHOT_NOTICE)
        terminal.send(typed('echo "MAIN""-MARK-1"'))
        terminal.expect(b"MAIN-MARK-1\r\n")
        terminal.expect(PROMPT)
        terminal.send(typed(
            "printf 'alt-line-%02d\\n' 1 2 3 > /tmp/zet-quit.txt; "
            "vim -u NONE -N -n /tmp/zet-quit.txt"))
        terminal.expect(ALT_ENTER)
        terminal.expect(b"alt-line-03")
        terminal.send(b":q\r")
        terminal.expect(ALT_LEAVE)
        terminal.expect(PROMPT)
        drain(terminal, 0.5)

        with link_cut(zet, terminal, iptables) as cut:
            wait_client_dropped(terminal, cut)
        after_cut = len(terminal.buffer)
        wait_recovered(terminal, after_cut)

        snapshot = session_stream(terminal, after_cut)
        assert ALT_ENTER not in snapshot, snapshot[:200]

        screen = render(snapshot)
        rows = rows_of(screen)
        assert not screen.alt_active
        assert "MAIN-MARK-1" in rows, rows
        assert not any("alt-line" in row for row in rows if "printf" not in row), rows
        assert last_row(rows).endswith("@zet-prompt$"), rows

        # And the session goes on in the main screen.
        terminal.send(typed('echo "af""ter=$((6*7))"'))
        terminal.expect(b"after=42")
        terminal.expect(PROMPT)
        terminal.send(typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
