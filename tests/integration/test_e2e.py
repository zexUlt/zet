"""The M4 criterion of design.md §17, end to end: zet and zet-agent over a
real ssh to a real sshd, the shell on a real pty, the network cut with
iptables.

Every scenario skips until both binaries are built (the `binaries`
fixture). The CLI and environment they are held to are in e2e.py.
"""

from __future__ import annotations

import os
import random
import signal
import subprocess
import uuid
from pathlib import Path

import pytest

from e2e import (ESCAPE_NOTICE, ESCAPE_STATUS, GAVE_UP_STATUS, LOST_NOTICE, PROMPT,
                 REGAINED_NOTICE, TCP_LISTEN, Terminal, Zet, command_of,
                 connection_to_agent, parent_of, real_uid_of, socket_inodes, sockets_of)
from harness import STEP_TIMEOUT_SECONDS, PtyProcess, wait_gone, wait_until

# The stream the cut tests check: `seq 1 STREAM_LINES`, 6.9 MB. More than
# RESEND_BUFFER_MAX (4 MiB), so the agent's shell meets backpressure while
# the connection is cut and the resend has more than one buffer to cover.
STREAM_LINES = 1_000_000

# Where in the stream a cut may land, in bytes received by the client.
CUT_RANGE = (512 * 1024, 5 * 1024 * 1024)

# The seed that picks the cut moment. Printed with every failure, and taken
# from the environment to reproduce one.
CUT_SEED = int(os.environ.get("ZET_E2E_SEED", "17"))

ORPHAN_TIMEOUT_S = 2


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


def _shell_pid(terminal: Terminal) -> int:
    # Spelled so that the echo of the command line does not match.
    terminal.send(_typed('echo "she""ll=$$"'))
    terminal.expect(b"shell=")
    return int(terminal.expect(b"\r\n").strip())


def _expected_stream() -> bytes:
    return b"".join(b"%d\n" % n for n in range(1, STREAM_LINES + 1))


def test_a_session_runs_a_command_and_ends_with_the_shells_status(zet: Zet, clean_bashrc):
    # sh records the terminal's mode before zet and after it: zet must leave
    # it as it found it.
    script = ('printf "before=%s\\n" "$(stty -g)"; "$@"; rc=$?; '
              'printf "rc=%s after=%s\\n" "$rc" "$(stty -g)"')
    argv = ["sh", "-c", script, "sh", *zet.argv(zet.agent_env())]
    terminal = Terminal(PtyProcess(argv, env=zet.env()))
    try:
        before = terminal.expect(b"\r\n")
        terminal.expect(PROMPT)
        terminal.send(_typed('echo "round""trip"'))
        terminal.expect(b"roundtrip")
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 3"))
        terminal.expect(b"rc=")
        after = terminal.expect(b"\r\n")
        assert after.startswith(b"3 "), after
        assert after.split(b"after=")[1].strip() == before.split(b"before=")[1].strip()
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_zet_exits_with_the_shells_status(zet: Zet, clean_bashrc):
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 7"))
        assert terminal.finish() == 7
    finally:
        terminal.kill()


def test_a_noisy_bashrc_does_not_break_the_bootstrap(zet: Zet, noisy_bashrc):
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_vim_edits_a_file(zet: Zet, clean_bashrc):
    path = Path(f"/tmp/zet-vim-{uuid.uuid4().hex}")
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed(f"vim -u NONE -N {path}"))
        # The status line of a file that does not exist yet.
        terminal.expect(b"[New")
        terminal.send(b"ihello zet\x1b:wq\r")
        terminal.expect(PROMPT)
        assert path.read_text() == "hello zet\n"
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
        path.unlink(missing_ok=True)


def test_htop_starts_and_quits(zet: Zet, clean_bashrc):
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed("htop"))
        terminal.expect(b"Load average")
        terminal.send(b"q")
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_the_agent_is_not_root_and_only_accepts(zet: Zet, clean_bashrc):
    """§15: nothing in a session connects anywhere but the peer. The agent
    listens and accepts; every connected socket it has shares the port it
    listens on, so none of them was dialled out."""
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        shell = _shell_pid(terminal)
        agent = parent_of(shell)
        assert command_of(agent) == "zet-agent"
        assert real_uid_of(agent) != 0
        sockets = sockets_of(agent, zet.host.user)
        listening = {s.local_port for s in sockets if s.state == TCP_LISTEN}
        assert len(listening) == 1, sockets
        assert all(s.local_port in listening for s in sockets), sockets
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def _cut_mid_stream(zet: Zet, iptables: str, rules_for, seed: int) -> None:
    """Runs the stream, cuts the client's connection once `seed` says, waits
    for the client to give the connection up, restores the path, and
    requires the stream on the far side of the cut to be exactly what was
    sent."""
    cut_at = random.Random(seed).randrange(*CUT_RANGE)
    context = f"seed {seed}, cut after {cut_at} bytes"
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed(
            f"printf '%s\\n' \"BE\"\"GIN\"; seq 1 {STREAM_LINES}; printf '%s\\n' \"EN\"\"D\""))
        terminal.expect(b"BEGIN\r\n")
        start = terminal.consumed
        while len(terminal.buffer) - start < cut_at:
            terminal.read_more()

        cut = connection_to_agent(terminal.pid, zet.host.port)
        rules = rules_for(cut.remote_port)
        for rule in rules:
            subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        try:
            # No descriptor says when another process closes a socket, hence
            # the poll: the cut connection leaving zet's descriptors is the
            # client noticing the cut and dropping it.
            wait_until(lambda: cut.inode not in socket_inodes(terminal.pid),
                       f"zet to drop the cut connection ({context})")
        finally:
            for rule in rules:
                subprocess.run([iptables, "-D", *rule], check=True,
                               timeout=STEP_TIMEOUT_SECONDS)

        terminal.expect(b"END\r\n")
        received = bytes(terminal.buffer[start:terminal.consumed - len(b"END\r\n")])
        # zet says so on stderr when the link goes and comes back, once each:
        # those lines are its own, not the session's.
        assert received.count(LOST_NOTICE) == 1, context
        assert received.count(REGAINED_NOTICE) == 1, context
        received = received.replace(LOST_NOTICE, b"", 1).replace(REGAINED_NOTICE, b"", 1)
        # The remote pty turns \n into \r\n once; a client that left OPOST on
        # would show a second \r.
        stream = received.replace(b"\r\n", b"\n")
        expected = _expected_stream()
        if stream != expected:
            first = next((i for i, (a, b) in enumerate(zip(stream, expected)) if a != b),
                         min(len(stream), len(expected)))
            pytest.fail(f"{context}: stream differs at byte {first} "
                        f"(got {len(stream)} bytes, want {len(expected)}): "
                        f"{stream[max(0, first - 40):first + 40]!r}")
        terminal.expect(PROMPT)
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0, context
    finally:
        terminal.kill()


def test_a_half_open_cut_recovers_byte_for_byte(zet: Zet, clean_bashrc, iptables):
    """Everything to and from the agent's port is dropped: no reset, no FIN,
    the connection just goes silent, the case only a keepalive finds."""
    def drop(port: int) -> list[list[str]]:
        return [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(port), "-j", "DROP"],
                ["INPUT", "-i", "lo", "-p", "tcp", "--sport", str(port), "-j", "DROP"]]
    _cut_mid_stream(zet, iptables, drop, CUT_SEED)


def test_a_reset_recovers_byte_for_byte(zet: Zet, clean_bashrc, iptables):
    """What the client sends to the agent's port is answered with a reset,
    the connection it had and every reconnect until the rule goes."""
    def reset(port: int) -> list[list[str]]:
        return [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(port),
                 "-j", "REJECT", "--reject-with", "tcp-reset"]]
    _cut_mid_stream(zet, iptables, reset, CUT_SEED + 1)


def test_an_orphaned_session_is_reaped(zet: Zet, clean_bashrc):
    terminal = zet.start(ZET_ORPHAN_TIMEOUT=str(ORPHAN_TIMEOUT_S))
    try:
        terminal.expect(PROMPT)
        shell = _shell_pid(terminal)
        agent = parent_of(shell)
        assert command_of(agent) == "zet-agent"
    finally:
        # The client dies without a word and never comes back.
        terminal.kill()
    # Both, so that a shell that lingers still says whether the agent did too.
    try:
        wait_gone(shell)
    finally:
        wait_gone(agent)


def _drop_both_ways(port: int) -> list[list[str]]:
    return [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(port), "-j", "DROP"],
            ["INPUT", "-i", "lo", "-p", "tcp", "--sport", str(port), "-j", "DROP"]]


def test_tilde_dot_quits_over_a_dead_link(zet: Zet, clean_bashrc, iptables):
    """Enter ~ . ends zet at once, with ssh's 255, even when the connection is
    gone and nothing would ever answer."""
    terminal = zet.start()
    rules: list[list[str]] = []
    try:
        terminal.expect(PROMPT)
        cut = connection_to_agent(terminal.pid, zet.host.port)
        rules = _drop_both_ways(cut.remote_port)
        for rule in rules:
            subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        terminal.send(b"\r~.")
        assert terminal.finish() == ESCAPE_STATUS
        assert ESCAPE_NOTICE in terminal.buffer
    finally:
        for rule in rules:
            subprocess.run([iptables, "-D", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        terminal.kill()


def test_the_client_gives_up_once_the_agent_is_gone(zet: Zet, clean_bashrc):
    """The agent dies; once reconnecting has failed for longer than the agent
    would have kept the session, zet stops trying and says why."""
    terminal = zet.start(ZET_ORPHAN_TIMEOUT=str(ORPHAN_TIMEOUT_S))
    try:
        terminal.expect(PROMPT)
        shell = _shell_pid(terminal)
        agent = parent_of(shell)
        os.kill(agent, signal.SIGKILL)
        assert terminal.finish() == GAVE_UP_STATUS
        assert b"gone" in terminal.tail()
    finally:
        terminal.kill()


def test_the_terminal_type_reaches_the_shell(zet: Zet, clean_bashrc):
    """ssh -T gives the far side no TERM; the bootstrap carries the client's
    (§4), and the shell has it rather than the agent's default."""
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed('echo "te""rm=$TERM"'))
        terminal.expect(b"term=")
        assert terminal.expect(b"\r\n").strip() == zet.env()["TERM"].encode()
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()
