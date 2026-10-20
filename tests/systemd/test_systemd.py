"""zet-muxd under real systemd (design.md §6, §16): the shipped units, socket
activation of both sockets, the confinement they and the mux set up, and a
service restart under live sessions.

The tests run in file order and share one socket unit: the first one needs
the service not yet started, the last one stops everything.
"""

from __future__ import annotations

import pwd
import socket
import stat
from pathlib import Path

import pytest

from units import (BIN_DIR, MUX_PORT, MUX_SOCKET, SECOND_USER, SERVICE_UNIT,
                   SOCKET_UNIT, Units, systemctl)
from e2e import (LOST_NOTICE, PROMPT, TCP_ESTABLISHED, TCP_LISTEN, Terminal, Zet,
                 parent_of, socket_inodes, sockets_of, tcp_sockets)
from harness import STEP_TIMEOUT_SECONDS
from sshhost import TEST_USER

MUX_USER = "zet"

# What /proc/<pid>/status says of a process under a seccomp filter, and of
# one that can gain no privileges.
SECCOMP_FILTER = "2"
NO_NEW_PRIVS = "1"

# The socket file as units/zet-muxd.socket asks systemd to make it.
SOCKET_MODE = 0o666
DIRECTORY_MODE = 0o755


def _typed(command: str) -> bytes:
    return command.encode() + b"\r"


def _shell_pid(terminal: Terminal) -> int:
    terminal.send(_typed('echo "she""ll=$$"'))
    terminal.expect(b"shell=")
    return int(terminal.expect(b"\r\n").strip())


def _round_trip(terminal: Terminal, word: str) -> None:
    half = len(word) // 2
    terminal.send(_typed(f'echo "{word[:half]}""{word[half:]}"'))
    terminal.expect(word.encode())
    terminal.expect(PROMPT)


def _uid(user: str) -> int:
    return pwd.getpwnam(user).pw_uid


def _zet(host) -> Zet:
    # The agent finds the mux at the default /run/zet/mux.sock: nothing in
    # its environment says where, as on a machine set up from the units.
    return Zet(client=BIN_DIR / "zet", agent=BIN_DIR / "zet-agent", host=host)


def _status(pid: int) -> dict[str, str]:
    fields = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        name, _, value = line.partition(":")
        fields[name] = value.strip()
    return fields


def _connection(terminal: Terminal, ssh_port: int):
    live = [s for s in sockets_of(terminal.pid)
            if s.state == TCP_ESTABLISHED and s.remote_port == MUX_PORT]
    assert len(live) == 1, live
    return live[0]


# The socket file systemd made, recorded before the service first runs.
_made_by_systemd: dict[str, int] = {}


def test_the_service_waits_for_its_first_connection(units: Units):
    assert units.state(SERVICE_UNIT) != "active", "started before anyone connected"
    # systemd made the agents' socket and its directory, as the unit says.
    made = MUX_SOCKET.lstat()
    assert stat.S_ISSOCK(made.st_mode)
    assert made.st_uid == _uid(MUX_USER)
    assert stat.S_IMODE(made.st_mode) == SOCKET_MODE
    directory = MUX_SOCKET.parent.lstat()
    assert directory.st_uid == 0
    assert stat.S_IMODE(directory.st_mode) == DIRECTORY_MODE
    _made_by_systemd["inode"] = made.st_ino

    with socket.create_connection(("127.0.0.1", MUX_PORT), timeout=STEP_TIMEOUT_SECONDS):
        pass
    units.wait_running()


def test_the_mux_runs_as_zet_confined(units: Units):
    pid = units.wait_running()
    status = _status(pid)
    assert int(status["Uid"].split()[0]) == _uid(MUX_USER)
    assert status["Seccomp"] == SECCOMP_FILTER
    assert status["NoNewPrivs"] == NO_NEW_PRIVS
    # Both of its sockets are the ones systemd bound: the TCP listener on the
    # port, and the unix socket whose file is still systemd's, not remade.
    held = socket_inodes(pid)
    listening = [s for s in tcp_sockets() if s.local_port == MUX_PORT and s.state == TCP_LISTEN]
    assert listening and all(s.inode in held for s in listening), (listening, held)
    assert MUX_SOCKET.lstat().st_ino == _made_by_systemd["inode"]


def test_two_users_share_the_units_port(units: Units, two_hosts):
    first, second = (_zet(host) for host in two_hosts)
    terminals = [first.start(), second.start()]
    try:
        for terminal, zet in zip(terminals, (first, second)):
            terminal.expect(PROMPT)
            agent = parent_of(_shell_pid(terminal))
            held = sockets_of(agent)
            assert not [s for s in held if s.state == TCP_LISTEN], held
            assert [s.local_port for s in held if s.state == TCP_ESTABLISHED] == [MUX_PORT], held
        uids = {uid for _, uid in units.registrations()}
        assert {_uid(TEST_USER), _uid(SECOND_USER)} <= uids
        for terminal, status in zip(terminals, (3, 4)):
            _round_trip(terminal, "through-systemd")
            terminal.send(_typed(f"exit {status}"))
        assert [t.finish() for t in terminals] == [3, 4]
    finally:
        for terminal in terminals:
            terminal.kill()


def test_a_service_restart_keeps_live_sessions(units: Units, two_hosts):
    zet = _zet(two_hosts[0])
    terminal = zet.start()
    try:
        terminal.expect(PROMPT)
        units.wait_registered(_uid(TEST_USER))
        before = _connection(terminal, zet.host.port)
        old = units.main_pid()
        seen = len(units.registrations())
        systemctl("restart", SERVICE_UNIT)
        assert units.wait_running() != old
        # Handed over before the restart, the connection never noticed it.
        _round_trip(terminal, "restarted-under-us")
        assert _connection(terminal, zet.host.port).inode == before.inode
        assert LOST_NOTICE not in terminal.buffer
        # And the agent registered with the new mux on its own.
        units.wait_registered(_uid(TEST_USER), after=seen)
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_stopping_the_units_closes_the_port(units: Units):
    systemctl("stop", SOCKET_UNIT, SERVICE_UNIT)
    assert units.state(SOCKET_UNIT) != "active"
    with pytest.raises(ConnectionRefusedError):
        socket.create_connection(("127.0.0.1", MUX_PORT), timeout=STEP_TIMEOUT_SECONDS)
