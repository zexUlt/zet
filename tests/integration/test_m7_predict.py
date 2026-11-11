"""M7 step 6 (design.md §16, "M7 — тестовые критерии", Интеграция): predictive
echo against a real agent under emulated latency (netem on lo, see the netem
fixture of conftest.py) and loss.

Judged on what the terminal shows, not on timing: the client's overlay log
(ZET_TEST_OVERLAY_LOG) says which bytes of its stdout are predictions, so the
agent's own stream is the terminal's bytes minus those, and "a prediction
appeared before the echo" is an ordering of offsets in one stream. Where time
decides anyway, the bound is generous.

Every session starts with `clear`, so the engine trusts the cursor column and
may predict (round B of §9; the cursor position report of round C is not
used here).
"""

from __future__ import annotations

import re
import uuid
from pathlib import Path

from e2e import PROMPT, REGAINED_NOTICE, Zet
from sshhost import TEST_USER
from m6_support import (ALT_ENTER, drain, link_cut, render, require_pyte, rows_of, typed,
                        wait_client_dropped)
from predict_support import Predicted, assert_no_stray_overlay, strip_overlay

LETTERS = "abcdefghijklmnopqrstuvwxyz"
CSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def type_keys(p: Predicted, text: str, gap: float) -> None:
    for letter in text:
        p.terminal.send(letter.encode())
        p.pump(gap)


def predicted_before_echo(p: Predicted, text: str, since: int) -> dict[str, bool | None]:
    """For every letter of `text`, typed after the terminal had received
    `since` bytes (with nothing in flight, so they are all the agent's or the
    overlay's of before): whether an
    overlay piece carrying it comes before the agent's echo of it in the
    stream (True), after it (False), or none does (None)."""
    entries = p.entries()
    stream = p.stream()
    agent = strip_overlay(stream, entries)
    line = since - sum(length for offset, length in entries if offset < since)
    echoed = agent[line:]
    pieces = []
    removed = 0
    for offset, length in entries:
        pieces.append((offset - removed, CSI.sub(b"", stream[offset:offset + length])))
        removed += length
    result: dict[str, bool | None] = {}
    for letter in text:
        raw = letter.encode()
        echo_at = line + echoed.index(raw) if raw in echoed else None
        first = next((at for at, glyphs in pieces if raw in glyphs and at >= line), None)
        if first is None:
            result[letter] = None
        else:
            result[letter] = echo_at is None or first <= echo_at
    return result


def converge(p: Predicted, lines: int = 6) -> None:
    """Types lines of letters, cleared with ^U, until the client has drawn a
    piece: the SRTT is then past the threshold. Each keystroke a second or
    more after the last probes the round trip."""
    for _ in range(lines):
        before = len(p.entries())
        type_keys(p, LETTERS, 0.3)
        p.terminal.send(b"\x15")
        p.pump(0.5)
        if len(p.entries()) > before:
            return
    raise AssertionError(f"no overlay piece in {lines} lines of typing")


def test_m7_adaptive_low_rtt_draws_nothing(zet: Zet, clean_bashrc, tmp_path):
    p = Predicted(zet, "adaptive", tmp_path)
    try:
        p.prime()
        type_keys(p, LETTERS, 0.1)
        p.settle()
        assert p.entries() == []
    finally:
        p.kill()


def test_m7_adaptive_on_off(zet: Zet, clean_bashrc, netem, tmp_path):
    p = Predicted(zet, "adaptive", tmp_path)
    try:
        p.prime()
        # Without delay not one piece, however much is typed.
        type_keys(p, LETTERS, 0.1)
        p.terminal.send(b"\x15")
        p.settle()
        assert p.entries() == []

        with netem("delay 50ms"):
            # Each keystroke after the first second probes the round trip;
            # pieces show up once the SRTT is past the threshold.
            converge(p, 4)
            p.pump(0.5)
            since = len(p.stream())
            type_keys(p, LETTERS, 0.3)
            p.pump(0.5)
            verdicts = predicted_before_echo(p, LETTERS, since)
            late = [letter for letter in LETTERS[-8:] if verdicts[letter] is not True]
            assert not late, (late, verdicts)
            p.terminal.send(b"\x15")
            p.settle()
        # Netem is off: the SRTT comes down by samples, one a second at most,
        # and the pieces stop.
        quiet_lines = 0
        for _ in range(12):
            before = len(p.entries())
            type_keys(p, LETTERS, 0.3)
            p.terminal.send(b"\x15")
            p.pump(0.5)
            quiet_lines = quiet_lines + 1 if len(p.entries()) == before else 0
            if quiet_lines >= 2:
                break
        assert quiet_lines >= 2, "pieces never stopped after the delay was lifted"
        count = len(p.entries())
        type_keys(p, LETTERS, 0.15)
        p.settle()
        assert len(p.entries()) == count
    finally:
        p.kill()


def test_m7_high_rtt_predicts_before_echo(zet: Zet, clean_bashrc, netem, tmp_path):
    require_pyte()
    p = Predicted(zet, "adaptive", tmp_path)
    try:
        p.prime()
        with netem("delay 60ms"):
            converge(p, 4)
            p.pump(0.5)
            since = len(p.stream())
            argument = "qwrtyzxbn"
            type_keys(p, "echo " + argument, 0.3)
            p.pump(0.5)
            verdicts = predicted_before_echo(p, argument, since)
            late = [letter for letter in argument[-5:] if verdicts[letter] is not True]
            assert not late, (late, verdicts)

            p.terminal.send(b"\r")
            p.terminal.expect(argument.encode() + b"\r\n")
            p.terminal.expect(PROMPT)
            p.settle()
        # Every piece was taken off again: the screen is the agent's.
        assert_no_stray_overlay(p)
        rows = rows_of(render(p.stream()))
        assert any(row.endswith("$ echo " + argument) for row in rows), rows
        assert argument in rows, rows
    finally:
        p.kill()


UNDERLINE = re.compile(rb"\x1b\[(?:[0-9;]*;)?4(?:;[0-9;]*)?m")


def test_m7_underline_above_threshold(zet: Zet, clean_bashrc, netem, tmp_path):
    """SRTT / 2 above 80 ms (RTT above 160 ms): the predictions are drawn
    underlined, and the screen shows it."""
    require_pyte()
    p = Predicted(zet, "adaptive", tmp_path)
    try:
        p.prime()
        with netem("delay 120ms"):
            found = None
            for _ in range(6):
                type_keys(p, LETTERS, 0.3)
                p.pump(0.5)
                stream = p.stream()
                for offset, length in reversed(p.entries()):
                    if UNDERLINE.search(stream[offset:offset + length]):
                        found = offset + length
                        break
                if found is not None:
                    break
                p.terminal.send(b"\x15")
                p.pump(0.5)
            assert found is not None, "no underlined piece in six lines at D=120 ms"
            screen = render(stream[:found])
            underlined = [screen.buffer[screen.cursor.y][x].data for x in range(screen.columns)
                          if screen.buffer[screen.cursor.y][x].underscore]
            assert underlined and all(c in LETTERS for c in underlined), underlined
            p.terminal.send(b"\x15")
            p.settle()
        assert_no_stray_overlay(p)
    finally:
        p.kill()


def test_m7_no_underline_below_threshold(zet: Zet, clean_bashrc, netem, tmp_path):
    """RTT 100 ms (SRTT / 2 = 50, over the show threshold, under the underline
    one): predictions are drawn and none of them is underlined."""
    p = Predicted(zet, "adaptive", tmp_path)
    try:
        p.prime()
        with netem("delay 50ms"):
            drawn = 0
            for _ in range(6):
                type_keys(p, LETTERS, 0.3)
                p.pump(0.5)
                stream = p.stream()
                for offset, length in p.entries():
                    piece = stream[offset:offset + length]
                    assert not UNDERLINE.search(piece), piece
                drawn = len(p.entries())
                p.terminal.send(b"\x15")
                p.pump(0.5)
            assert drawn > 0, "nothing was predicted at D=50 ms, the control is vacuous"
            p.settle()
        assert_no_stray_overlay(p)
    finally:
        p.kill()


def test_m7_password_never_shown(zet: Zet, clean_bashrc, netem, tmp_path):
    """The program turns the echo off and reads a password. The client is
    asked for predictions always, and the epoch of the typed line is not
    confirmed, so not a byte of the password is written to its terminal."""
    p = Predicted(zet, "always", tmp_path)
    try:
        p.prime()
        t = p.terminal
        with netem("delay 50ms"):
            # An echo first, so that an epoch is confirmed and predictions
            # are on the screen for ordinary typing.
            type_keys(p, "echo ok", 0.15)
            t.send(b"\r")
            t.expect(b"ok\r\n")
            t.expect(PROMPT)
            p.settle()
            type_keys(p, "stty -echo; printf 'Pass''word: '; read -r x; stty echo; echo; echo \"len=${#x}\"", 0.15)
            t.send(b"\r")
            t.expect(b"Password: ")
            p.pump(0.5)
            pieces = len(p.entries())
            assert pieces > 0, "ordinary typing was not predicted: the test would prove nothing"
            mark = len(t.buffer)

            for letter in "hunter2":
                t.send(letter.encode())
                p.pump(0.2)
            t.send(b"\r")
            t.expect(b"len=7\r\n")
            t.expect(PROMPT)
            p.settle()
        assert len(p.entries()) == pieces, p.entries()[pieces:]
        assert b"hunter2" not in t.buffer
        # Nothing of the typed bytes reached the terminal but the shell's own
        # words: "len=7" and the next prompt.
        shown = bytes(t.buffer[mark:])
        for letter in "hun2":
            assert letter.encode() not in CSI.sub(b"", shown).replace(b"len=7", b"").replace(
                b"zettest@zet-prompt$ ", b""), shown
    finally:
        p.kill()


def test_m7_never_byte_identical(zet: Zet, clean_bashrc, netem, tmp_path):
    """--predict=never under latency: no overlay, and the terminal
    receives exactly what the shell wrote."""
    p = Predicted(zet, "never", tmp_path)
    try:
        p.prime()
        t = p.terminal
        mark = len(t.buffer)
        with netem("delay 50ms"):
            type_keys(p, "seq 1 30", 0.1)
            t.send(b"\r")
            t.expect(b"30\r\n")
            t.expect(PROMPT)
            p.settle()
        got = bytes(t.buffer[mark:])
        want = (b"seq 1 30\r\n\x1b[?2004l\r" + b"".join(b"%d\r\n" % i for i in range(1, 31))
                + b"\x1b[?2004h" + TEST_USER.encode() + b"@zet-prompt$ ")
        assert got == want
        t.send(typed("exit 0"))
        assert t.finish() == 0
        assert p.entries() == []
    finally:
        p.kill()


def test_m7_loss_and_cut_leave_no_stray_overlay(zet: Zet, clean_bashrc, netem, iptables, tmp_path):
    """Latency with jitter (so packets overtake one another), loss, and then
    a cut of the link while predictions are on the screen and keys are being
    typed. After the reconnect nothing of the overlay is left: the screen is
    what the agent's own stream draws, and the line holds every key."""
    require_pyte()
    p = Predicted(zet, "always", tmp_path)
    try:
        p.prime()
        t = p.terminal
        with netem("delay 40ms 20ms loss 2% reorder 25% 50% seed 7"):
            # Up to the pieces being drawn: a confirmed epoch.
            first = LETTERS[:10]
            type_keys(p, first, 0.25)
            pieces = len(p.entries())
            assert pieces > 0, "nothing was predicted before the cut"

            second = LETTERS[10:20]
            after_cut = len(t.buffer)
            with link_cut(zet, t, iptables) as cut:
                # Keys typed into a client that has not noticed yet, then
                # into one that knows it is disconnected.
                type_keys(p, second[:5], 0.2)
                wait_client_dropped(t, cut)
                type_keys(p, second[5:], 0.2)
            t.read_until(lambda: REGAINED_NOTICE in t.buffer[after_cut:], "the reconnect")
            type_keys(p, LETTERS[20:], 0.25)
            p.settle(1.0)
        # The whole line is on the screen, once, and the overlay is gone.
        assert_no_stray_overlay(p)
        # Run, the line is the shell's complaint about a command of that name:
        # every key arrived, once and in order.
        t.send(b"\r")
        t.expect(b"command not found")
        p.settle()
        agent = p.agent_stream()
        assert b"bash: " + LETTERS.encode() + b": command not found" in agent
    finally:
        p.kill()


def ground_positions(data: bytes) -> set[int]:
    """The offsets in `data` at which a terminal's parser is in Ground and
    not in the middle of a UTF-8 character."""
    ground: set[int] = set()
    state = "ground"
    pending = 0
    for at, byte in enumerate(data):
        if state == "ground" and pending == 0:
            ground.add(at)
        if byte in (0x18, 0x1A):
            state, pending = "ground", 0
            continue
        if state in ("oscesc", "stresc"):
            if byte == 0x5C:
                state = "ground"
                continue
            state = "esc"
        if state == "ground":
            if pending:
                pending -= 1
            elif byte == 0x1B:
                state = "esc"
            elif byte >= 0xC0:
                pending = 1 if byte < 0xE0 else 2 if byte < 0xF0 else 3
        elif state == "esc":
            if byte == 0x5B:
                state = "csi"
            elif byte == 0x5D:
                state = "osc"
            elif byte in (0x50, 0x58, 0x5E, 0x5F):
                state = "str"
            elif not 0x20 <= byte <= 0x2F:
                state = "ground"
        elif state == "csi":
            if byte == 0x1B:
                state = "esc"
            elif 0x40 <= byte <= 0x7E:
                state = "ground"
        elif state == "osc":
            if byte == 0x07:
                state = "ground"
            elif byte == 0x1B:
                state = "oscesc"
        elif state == "str":
            if byte == 0x1B:
                state = "stresc"
    if state == "ground" and pending == 0:
        ground.add(len(data))
    return ground


# A program that alternates two phases. In the first it echoes what is typed
# at once, so that the client confirms an epoch and its predictions are drawn.
# In the second it writes control strings in pieces with pauses and echoes
# nothing, while the client goes on typing: the predictions made then may be
# drawn only between the strings, never inside one. Keys arrive in raw mode,
# so the line discipline echoes nothing of them.
SEQUENCE_PROGRAM = r"""
import os, select, sys, termios, time, tty
saved = termios.tcgetattr(0)
tty.setraw(0)
out = sys.stdout.buffer
def say(data):
    out.write(data); out.flush()
strings = [
    [b"\x1b[3", b"8;2;1", b"0;20;3", b"0m"],
    [b"\x1b]0;a long ", b"window ti", b"tle\x07"],
    [b"\x1bP1$r", b"0 q", b"\x1b\\"],
    [b"\x1b]8;;http://exa", b"mple.org/", b"\x1b\\"],
    [b"\x1b[1;", b"4", b"m", b"\x1b[0", b"m"],
]
say(b"GO\r\n")
for _ in range(6):
    say(b"\r")
    end = time.monotonic() + 0.7
    while time.monotonic() < end:
        if select.select([0], [], [], 0.05)[0]:
            say(os.read(0, 64))
    for string in strings:
        for part in string:
            say(part)
            time.sleep(0.08)
    termios.tcflush(0, termios.TCIFLUSH)
say(b"\r\nDONE\r\n")
termios.tcsetattr(0, termios.TCSADRAIN, saved)
"""


def test_m7_overlay_never_splits_sequence(zet: Zet, clean_bashrc, netem, tmp_path):
    p = Predicted(zet, "always", tmp_path)
    program = Path(f"/tmp/zet-sequences-{uuid.uuid4().hex}.py")
    program.write_text(SEQUENCE_PROGRAM)
    program.chmod(0o644)
    try:
        p.prime()
        t = p.terminal
        with netem("delay 40ms"):
            t.send(typed(f"python3 {program}"))
            t.expect(b"GO\r\n")
            # Keys all through the program's run, which takes about 12 s.
            for count in range(100):
                t.send(LETTERS[count % 26].encode())
                p.pump(0.12)
            t.expect(b"DONE\r\n", deadline=60)
            t.expect(PROMPT)
            p.settle()
        entries = p.entries()
        assert len(entries) >= 5, entries
        stream = p.stream()
        agent = strip_overlay(stream, entries)
        # The agent's stream carries every string whole.
        for string in (b"\x1b[38;2;10;20;30m", b"\x1b]0;a long window title\x07",
                       b"\x1bP1$r0 q\x1b\\", b"\x1b]8;;http://example.org/\x1b\\"):
            assert string in agent
        ground = ground_positions(agent)
        removed = 0
        for offset, length in entries:
            assert offset - removed in ground, (offset - removed, agent[offset - removed - 20:offset - removed + 20])
            removed += length
    finally:
        program.unlink(missing_ok=True)
        p.kill()


NO_ECHO_ACK_NOTICE = b"zet: predictive echo is off: the agent does not support it\r\n"


def test_m7_compat_old_peer(zet: Zet, clean_bashrc, netem, tmp_path):
    """A client whose protocol ceiling is 1 gets no EchoAck from the agent:
    with --predict=always it says so on stderr, and draws and mirrors
    nothing; the stream is the agent's byte for byte."""
    p = Predicted(zet, "always", tmp_path, client_env={"ZET_TEST_VERSION_CEILING": "1"})
    try:
        p.prime()
        t = p.terminal
        mark = len(t.buffer)
        with netem("delay 50ms"):
            type_keys(p, "seq 1 30", 0.1)
            t.send(b"\r")
            t.expect(b"30\r\n")
            t.expect(PROMPT)
            p.settle()
        got = bytes(t.buffer[mark:])
        want = (b"seq 1 30\r\n\x1b[?2004l\r" + b"".join(b"%d\r\n" % i for i in range(1, 31))
                + b"\x1b[?2004h" + TEST_USER.encode() + b"@zet-prompt$ ")
        assert got == want
        assert t.buffer.count(NO_ECHO_ACK_NOTICE) == 1, t.buffer[:300]
        assert p.entries() == []
    finally:
        p.kill()


def cells_of(screen) -> list[list[tuple]]:
    return [[(c.data, c.fg, c.bg, c.bold, c.underscore, c.reverse)
             for c in (screen.buffer[y][x] for x in range(screen.columns))]
            for y in range(screen.lines)]


def test_m7_mirror_fidelity(zet: Zet, clean_bashrc, netem, tmp_path):
    """The client's mirror of the terminal (ZET_TEST_MIRROR_DUMP) is what a
    terminal shows after the same bytes, cell for cell, after a session of
    ordinary typing with backspaces, coloured listings and vim on the main
    screen, with predictions drawn all along."""
    require_pyte()
    p = Predicted(zet, "always", tmp_path, mirror_dump=True)
    try:
        p.prime()
        t = p.terminal
        with netem("delay 40ms"):
            type_keys(p, "echo mirror-check", 0.15)
            t.send(b"\x7f\x7f\x7f\x7f\x7f\r")
            t.expect(b"mirror-\r\n")
            t.expect(PROMPT)
            t.send(typed("ls --color=always /etc | head -n 12"))
            t.expect(PROMPT)
            t.send(typed("printf 'one\\ntwo\\n' > /tmp/zet-mirror.txt; "
                         "vim -u NONE -N -n --cmd 'set t_ti= t_te=' /tmp/zet-mirror.txt"))
            t.expect(b"two")
            p.pump(0.5)
            type_keys(p, "ihello", 0.15)
            t.send(b"\x1b")
            p.pump(0.3)
            t.send(b":q!\r")
            t.expect(PROMPT)
            type_keys(p, "xyz", 0.15)
            t.send(b"\x7f\r")
            t.expect(PROMPT)
            p.settle()
            t.send(typed("exit 0"))
            assert t.finish() == 0
        assert ALT_ENTER not in t.buffer
        assert p.dump.exists()
        mirror = render(p.dump.read_bytes())
        terminal = render(p.stream())
        assert rows_of(mirror) == rows_of(terminal)
        assert cells_of(mirror) == cells_of(terminal)
        assert assert_cursor(mirror, terminal)
    finally:
        p.kill()


def assert_cursor(a, b) -> bool:
    assert (a.cursor.x, a.cursor.y) == (b.cursor.x, b.cursor.y)
    return True


def test_m7_vim_mispredict_repaired(zet: Zet, clean_bashrc, netem, tmp_path):
    """In vim `q` leaves insert mode, so the predicted `q` is wrong. It is
    drawn before the agent answers and is gone after: the screen is the
    agent's."""
    require_pyte()
    p = Predicted(zet, "always", tmp_path)
    try:
        p.prime()
        t = p.terminal
        with netem("delay 100ms"):
            t.send(typed("vim -u NONE -N -n --cmd 'set t_ti= t_te=' --cmd 'inoremap q <Esc>'"))
            t.expect(b"~")
            p.pump(1.0)
            t.send(b"i")
            t.expect(b"INSERT")
            p.pump(0.5)
            type_keys(p, "abcdef", 0.4)
            p.pump(0.6)
            since = len(p.stream())
            t.send(b"q")
            p.pump(1.0)
            verdict = predicted_before_echo(p, "q", since)
            assert verdict["q"] is True, verdict
            p.settle()
        assert_no_stray_overlay(p)
        rows = rows_of(render(p.stream()))
        assert any(row.startswith("abcdef") for row in rows), rows
        assert not any("abcdefq" in row for row in rows), rows
        with netem("delay 100ms"):
            t.send(b":q!\r")
            t.expect(PROMPT)
    finally:
        p.kill()
