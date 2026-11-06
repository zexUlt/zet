"""The M6 split of design.md §9 step 2, end to end: ZET_TEST_FORCE_PATH picks
the resume path regardless of the replay size, so a short cut is recovered
either by replay (A) or by a GridSnapshot (B)."""

from __future__ import annotations

import subprocess

from e2e import PROMPT, REGAINED_NOTICE, Terminal, Zet, connection_to_agent, socket_inodes
from harness import STEP_TIMEOUT_SECONDS, Timeout, wait_until

# What a snapshot opens with (grid/snapshot.cpp): soft reset, main screen,
# erase.
SNAPSHOT_PREAMBLE = b"\x1b[!p\x1b[?1049l\x1b[2J"

# What the client prints once a snapshot is applied.
SNAPSHOT_NOTICE = b"zet: screen restored"


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


def _echo(terminal: Terminal, name: str) -> None:
    # Spelled so that the echo of the command line does not match.
    terminal.send(_typed(f'echo "{name[:2]}""{name[2:]}=$((6*7))"'))
    terminal.expect(f"{name}=42".encode())
    terminal.expect(PROMPT)


def _short_cut(zet: Zet, iptables: str, force: str) -> bytes:
    """Cuts the connection half-open after a short exchange, lifts the cut
    once the client dropped it, requires the session to work again, and
    returns what the terminal showed from the cut on."""
    terminal = zet.start(ZET_TEST_FORCE_PATH=force)
    try:
        if force == "B":
            # The first attach is a resume too, so B takes it as well: the
            # screen arrives as a snapshot, whose prompt has no trailing space.
            terminal.expect(SNAPSHOT_NOTICE)
        else:
            terminal.expect(PROMPT)
        _echo(terminal, "before")
        start = terminal.consumed

        cut = connection_to_agent(terminal.pid, zet.host.port)
        rules = [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(cut.remote_port), "-j", "DROP"],
                 ["INPUT", "-i", "lo", "-p", "tcp", "--sport", str(cut.remote_port), "-j", "DROP"]]
        for rule in rules:
            subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        try:
            def dropped() -> bool:
                while True:
                    try:
                        terminal.read_more(0)
                    except Timeout:
                        break
                return cut.inode not in socket_inodes(terminal.pid)

            wait_until(dropped, "zet to drop the cut connection")
        finally:
            for rule in rules:
                subprocess.run([iptables, "-D", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)

        terminal.expect(REGAINED_NOTICE)
        # The session works after the recovery, on either path.
        _echo(terminal, "after")
        shown = bytes(terminal.buffer[start:])
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
        return shown
    finally:
        terminal.kill()


def test_m6_path_b_only(zet: Zet, clean_bashrc, iptables):
    shown = _short_cut(zet, iptables, "B")
    assert shown.count(SNAPSHOT_PREAMBLE) == 1, shown


def test_m6_path_a_only(zet: Zet, clean_bashrc, iptables):
    shown = _short_cut(zet, iptables, "A")
    assert shown.count(SNAPSHOT_PREAMBLE) == 0, shown
