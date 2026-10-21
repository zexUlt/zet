"""The native macOS zet against the Linux zet-agent in the host container,
over real ssh and a real TCP path (design.md §12, §16).

The subset of the Linux end-to-end suite that says something about the
client: a session starts, runs a command and ends with the shell's status
with the terminal put back; the client's TERM reaches the shell; a cut in
the middle of a stream recovers byte for byte; and ~. quits over a dead
link. What is the agent's alone — vim and htop on its pty, the orphan
timeout, its sockets — the Linux suite covers.
"""

from __future__ import annotations

import os
import random

import pytest

from e2e import (ESCAPE_NOTICE, ESCAPE_STATUS, KEEPALIVE_INTERVAL_MS, LOST_NOTICE,
                 PEER_SILENCE_TIMEOUT_MS, PROFILE, PROMPT, RECONNECT_MAX_MS,
                 REGAINED_NOTICE, Terminal)
from harness import PtyProcess

# The stream the cut checks: as in the Linux suite, more than the agent's
# resend buffer, so the cut meets backpressure on the far side.
STREAM_LINES = 1_000_000

# Where in the stream the cut lands, in bytes the client has received.
CUT_RANGE = (512 * 1024, 5 * 1024 * 1024)

CUT_SEED = int(os.environ.get("ZET_E2E_SEED", "23"))

DEFAULT_ORPHAN_TIMEOUT_S = 20


# PENDIN in macOS's lflag: "retype pending input", set by the tty driver
# itself when canonical mode comes back with input it has not yet echoed.
# It is the driver's state, not a mode zet saved or should restore.
PENDIN = 0x20000000


def _mode(stty: bytes) -> bytes:
    """`stty -g` output with the driver's own PENDIN bit masked out."""
    fields = []
    for field in stty.strip().split(b":"):
        if field.startswith(b"lflag="):
            value = int(field[len(b"lflag="):], 16) & ~PENDIN
            field = b"lflag=%x" % value
        fields.append(field)
    return b":".join(fields)


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


class MacZet:
    """The native client, set up to reach the container host."""

    def __init__(self, client, host):
        self.client = client
        self.host = host

    def env(self) -> dict[str, str]:
        env = dict(os.environ)
        env.update({
            "ZET_KEEPALIVE_INTERVAL": str(KEEPALIVE_INTERVAL_MS),
            "ZET_PEER_SILENCE_TIMEOUT": str(PEER_SILENCE_TIMEOUT_MS),
            "ZET_RECONNECT_MAX": str(RECONNECT_MAX_MS),
            "TERM": "xterm-256color",
        })
        return env

    def argv(self) -> list[str]:
        # No --agent: the client uses ZET_AGENT_DIR (delivery mode). The
        # remote cache persists across tests in the session; the first test
        # pays the delivery cost and subsequent ones skip it.
        settings = " ".join(f"{name}={value}" for name, value in {
            "ZET_KEEPALIVE_INTERVAL": KEEPALIVE_INTERVAL_MS,
            "ZET_PEER_SILENCE_TIMEOUT": PEER_SILENCE_TIMEOUT_MS,
            "ZET_ORPHAN_TIMEOUT": DEFAULT_ORPHAN_TIMEOUT_S,
            "ASAN_OPTIONS": "detect_leaks=0",
        }.items())
        return [str(self.client),
                "--ssh-arg", "-F", "--ssh-arg", str(self.host.config),
                "--ssh-arg", f"-oSetEnv={settings}", self.host.alias]

    def start(self) -> Terminal:
        return Terminal(PtyProcess(self.argv(), env=self.env()))


@pytest.fixture
def zet(macos_zet, clean_bashrc, agent_dir) -> MacZet:  # agent_dir skips if ZET_AGENT_DIR unset
    clean_bashrc.write_home_file(".bash_profile", PROFILE)
    return MacZet(macos_zet, clean_bashrc)


def _agent_port_rules(host, target: str) -> list[list[str]]:
    """Rules that cut every connection to an agent on the host, both ways.
    The client's own port is not known from here without /proc, and the host
    publishes only the agent's range, so the whole range is cut."""
    ports = f"{host.agent_ports.start}:{host.agent_ports.stop - 1}"
    return [["INPUT", "-i", "eth0", "-p", "tcp", "--dport", ports, "-j", target],
            ["OUTPUT", "-o", "eth0", "-p", "tcp", "--sport", ports, "-j", target]]


def _apply(host, rules: list[list[str]], action: str) -> None:
    for rule in rules:
        done = host.exec(["iptables", action, *rule])
        assert done.returncode == 0, done.stderr


def test_a_session_runs_a_command_and_ends_with_the_shells_status(zet):
    script = ('printf "before=%s\\n" "$(stty -g)"; "$@"; rc=$?; '
              'printf "rc=%s after=%s\\n" "$rc" "$(stty -g)"')
    terminal = Terminal(PtyProcess(["sh", "-c", script, "sh", *zet.argv()], env=zet.env()))
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
        assert _mode(after.split(b"after=")[1]) == _mode(before.split(b"before=")[1])
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_the_terminal_type_reaches_the_shell(zet):
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


def test_a_cut_recovers_byte_for_byte(zet, iptables):
    """DROP both ways on the agent's ports: half-open, which only the
    client's own liveness timer notices. Lifted once it says it lost the
    link, and the stream must arrive exactly as sent."""
    cut_at = random.Random(CUT_SEED).randrange(*CUT_RANGE)
    context = f"seed {CUT_SEED}, cut after {cut_at} bytes"
    rules = _agent_port_rules(iptables, "DROP")
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        terminal.send(_typed(
            f"printf '%s\\n' \"BE\"\"GIN\"; seq 1 {STREAM_LINES}; printf '%s\\n' \"EN\"\"D\""))
        terminal.expect(b"BEGIN\r\n")
        start = terminal.consumed
        while len(terminal.buffer) - start < cut_at:
            terminal.read_more()

        _apply(iptables, rules, "-I")
        try:
            # The client's notice is the event: it prints it as it drops the
            # connection its liveness timer gave up on.
            while terminal.buffer.find(LOST_NOTICE, start) < 0:
                terminal.read_more()
        finally:
            _apply(iptables, rules, "-D")

        terminal.expect(b"END\r\n")
        received = bytes(terminal.buffer[start:terminal.consumed - len(b"END\r\n")])
        assert received.count(LOST_NOTICE) == 1, context
        assert received.count(REGAINED_NOTICE) == 1, context
        received = received.replace(LOST_NOTICE, b"", 1).replace(REGAINED_NOTICE, b"", 1)
        stream = received.replace(b"\r\n", b"\n")
        expected = b"".join(b"%d\n" % n for n in range(1, STREAM_LINES + 1))
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


def test_tilde_dot_quits_over_a_dead_link(zet, iptables):
    rules = _agent_port_rules(iptables, "DROP")
    terminal = zet.start()
    applied = False
    try:
        terminal.expect(PROMPT)
        _apply(iptables, rules, "-I")
        applied = True
        terminal.send(b"\r~.")
        assert terminal.finish() == ESCAPE_STATUS
        assert ESCAPE_NOTICE in terminal.buffer
    finally:
        if applied:
            _apply(iptables, rules, "-D")
        terminal.kill()
