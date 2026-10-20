"""The M5 criterion of design.md §17, end to end: zet-muxd on one port for
every user's sessions, handing each connection to its agent (§6).

- two sessions of two users through the one port;
- a local unprivileged user can neither read, nor inject into, nor bring
  down someone else's session — nor the mux;
- restarting the mux does not end a session;
- without a mux the agent falls back to its own port, as in M4;
- through the mux an unknown sid looks like a known one (§5, rule two).

Every scenario skips until zet, zet-agent and zet-muxd are built. The
mux's contract is in mux.py, the client's and the agent's in e2e.py.
"""

from __future__ import annotations

import os
import pwd
import secrets
import signal
import socket
import subprocess
from pathlib import Path

import pytest

from e2e import (LOST_NOTICE, PROFILE, PROMPT, REGAINED_NOTICE, TCP_ESTABLISHED,
                 TCP_LISTEN, Terminal, Zet, connection_to_agent, parent_of,
                 socket_inodes, sockets_of)
from harness import STEP_TIMEOUT_SECONDS, wait_until
from mux import (CLEARTEXT_EPOCH, INTRUDER_USER, MUX_USER, REGISTRATION_SECRET_SIZE,
                 SESSION_ID_SIZE, TYPE_CHALLENGE, HEADER, Mux, auth, find_muxd, hello,
                 install_muxd, listener, mux_register, prepare_mux_dir, read_frame,
                 read_to_end)
from sshhost import (CLEAN_BASHRC, TEST_USER, SshHost, authorize, ensure_user,
                     make_keys, start_sshd, stop_sshd, write_client_config)

# The second user with a session of their own.
SECOND_USER = "zettest2"

# Where zet-muxd is installed for its user to run.
MUX_BIN_DIR = Path("/opt/zet-mux")

# How many connections the floods open: well past any limit a mux would set
# per uid or per address, and still quick to open on a CI runner.
FLOOD = 200


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


def _send_regardless(conn: socket.socket, data: bytes) -> None:
    """Sends what an intruder sends. The mux may hang up on it first, and is
    right to: that is not the test failing."""
    try:
        conn.sendall(data)
    except (BrokenPipeError, ConnectionResetError):
        pass


def _uid(user: str) -> int:
    return pwd.getpwnam(user).pw_uid


def _drop_both_ways(port: int) -> list[list[str]]:
    return [["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(port), "-j", "DROP"],
            ["INPUT", "-i", "lo", "-p", "tcp", "--sport", str(port), "-j", "DROP"]]


def _cut_and_reconnect(terminal: Terminal, zet: Zet, iptables: str, port: int) -> None:
    """Cuts the client's connection until it gives it up, then lets it back:
    the reconnect goes through whatever listens on `port` now."""
    cut = connection_to_agent(terminal.pid, zet.host.port)
    rules = _drop_both_ways(port)
    for rule in rules:
        subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
    try:
        # No event for another process closing a socket, hence the poll.
        wait_until(lambda: cut.inode not in socket_inodes(terminal.pid),
                   "zet to drop the cut connection")
    finally:
        for rule in rules:
            subprocess.run([iptables, "-D", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
    terminal.expect(REGAINED_NOTICE)


# ─── Fixtures ────────────────────────────────────────────────────────────────


@pytest.fixture(scope="module")
def muxd(binaries) -> Path:
    built = find_muxd()
    if built is None:
        pytest.skip(f"zet-muxd not built under {os.environ.get('ZET_BUILD_DIR', 'build/musl-dev')} yet")
    ensure_user(MUX_USER, shell="/sbin/nologin")
    return install_muxd(built, MUX_BIN_DIR)


@pytest.fixture(scope="module")
def two_hosts(tmp_path_factory, muxd) -> tuple[SshHost, SshHost]:
    """One sshd that lets both test users in, and a client config for each."""
    work = tmp_path_factory.mktemp("sshd-m5")
    first = ensure_user(TEST_USER)
    second = ensure_user(SECOND_USER)
    keys = make_keys(work, first)
    authorize(second, (work / "client_key.pub").read_text())
    ensure_user(INTRUDER_USER)
    sshd, port = start_sshd(work, keys, Path(first.pw_dir), "127.0.0.1",
                            users=(TEST_USER, SECOND_USER))
    hosts = []
    for entry in (first, second):
        config = write_client_config(
            work / f"ssh_config.{entry.pw_name}", hostname="127.0.0.1", port=port,
            identity=keys.client, known_hosts=work / f"known_hosts.{entry.pw_name}",
            host_public_key=(work / "host_key.pub").read_text(),
            user=entry.pw_name, alias=f"zet-m5-{entry.pw_name}")
        host = SshHost(config=config, alias=f"zet-m5-{entry.pw_name}",
                       user=entry.pw_name, home=Path(entry.pw_dir), port=port)
        host.write_home_file(".bashrc", CLEAN_BASHRC)
        host.write_home_file(".bash_profile", PROFILE)
        hosts.append(host)
    try:
        yield hosts[0], hosts[1]
    finally:
        stop_sshd(sshd)


@pytest.fixture
def mux(muxd) -> Mux:
    directory = prepare_mux_dir()
    log = directory / "mux.log"
    running = Mux(binary=muxd, socket_path=directory / "mux.sock", log_path=log,
                  port=0, listener=listener())
    running.port = running.listener.getsockname()[1]
    running.start()
    try:
        yield running
    finally:
        running.stop(signal.SIGKILL)
        running.listener.close()


def _zet(binaries, installed_agent: Path, host: SshHost) -> Zet:
    return Zet(client=binaries.client, agent=installed_agent, host=host)


def _start(zet: Zet, mux: Mux | None, **agent_env: str) -> Terminal:
    if mux is not None:
        agent_env.setdefault("ZET_MUX_SOCKET", str(mux.socket_path))
    return zet.start(**agent_env)


# ─── Scenarios ───────────────────────────────────────────────────────────────


def test_two_users_share_one_port(binaries, installed_agent, two_hosts, mux):
    first = _zet(binaries, installed_agent, two_hosts[0])
    second = _zet(binaries, installed_agent, two_hosts[1])
    terminals = [_start(first, mux), _start(second, mux)]
    try:
        for terminal, zet, status in zip(terminals, (first, second), (3, 4)):
            terminal.expect(PROMPT)
            agent = parent_of(_shell_pid(terminal))
            # The agent listens on nothing of its own: its one TCP socket is
            # the connection the mux handed it, on the mux's port.
            held = sockets_of(agent, zet.host.user)
            assert not [s for s in held if s.state == TCP_LISTEN], held
            live = [s for s in held if s.state == TCP_ESTABLISHED]
            assert [s.local_port for s in live] == [mux.port], held
            assert connection_to_agent(terminal.pid, zet.host.port).remote_port == mux.port
        uids = {uid for _, uid in mux.registrations()}
        assert {_uid(TEST_USER), _uid(SECOND_USER)} <= uids
        for terminal, status in zip(terminals, (3, 4)):
            _round_trip(terminal, "shared-port")
            terminal.send(_typed(f"exit {status}"))
        assert [t.finish() for t in terminals] == [3, 4]
    finally:
        for terminal in terminals:
            terminal.kill()


def test_an_intruder_cannot_take_a_session(binaries, installed_agent, two_hosts, mux, iptables):
    """A local user who read the victim's sid registers what they can — a
    made-up R, and R spelled from the sid — and still never receives the
    victim's connection, not even across a reconnect."""
    victim = _zet(binaries, installed_agent, two_hosts[0])
    terminal = _start(victim, mux)
    intruders: list[socket.socket] = []
    try:
        terminal.expect(PROMPT)
        sid = bytes.fromhex(mux.wait_registered(_uid(TEST_USER)))
        assert len(sid) == SESSION_ID_SIZE
        for guess in (secrets.token_bytes(REGISTRATION_SECRET_SIZE),
                      sid + sid,
                      sid + bytes(REGISTRATION_SECRET_SIZE - SESSION_ID_SIZE)):
            conn = mux.connect_local(INTRUDER_USER)
            _send_regardless(conn, mux_register(guess))
            intruders.append(conn)
        # Whatever the mux made of those, none of them is the victim's sid.
        taken = [s for s, uid in mux.registrations() if uid == _uid(INTRUDER_USER)]
        assert sid.hex() not in taken

        # The victim reconnects; the connection must go to the victim's agent.
        _cut_and_reconnect(terminal, victim, iptables, mux.port)
        _round_trip(terminal, "still-mine")
        for conn in intruders:
            conn.setblocking(False)
            try:
                _, fds, _, _ = socket.recv_fds(conn, 4096, 4)
            except BlockingIOError:
                fds = []
            for fd in fds:
                os.close(fd)
            assert not fds, "the mux handed an intruder a connection"
        # Nobody but the victim's agent holds the victim's connection: not the
        # mux, which let go of it, and not a process of the intruder's.
        live = connection_to_agent(terminal.pid, victim.host.port)
        suspects = [mux.process.pid] + [pid for pid in _pids()
                                         if _owner(pid) == INTRUDER_USER]
        holders = [pid for pid in suspects
                   if _holds(pid, live.remote_port, live.local_port)]
        assert not holders, holders
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        for conn in intruders:
            conn.close()
        terminal.kill()


def test_garbage_does_not_bring_the_mux_down(binaries, installed_agent, two_hosts, mux):
    first = _zet(binaries, installed_agent, two_hosts[0])
    second = _zet(binaries, installed_agent, two_hosts[1])
    terminal = _start(first, mux)
    opened: list[socket.socket] = []
    try:
        terminal.expect(PROMPT)
        # On the agents' socket: noise, a header that promises more than it
        # brings, a length past any frame, and registrations by the hundred.
        for payload in (secrets.token_bytes(512),
                        HEADER.pack(64, CLEARTEXT_EPOCH) + b"\x20abc",
                        HEADER.pack(0xFFFFFFFF, CLEARTEXT_EPOCH),
                        b""):
            conn = mux.connect_local(INTRUDER_USER)
            if payload:
                _send_regardless(conn, payload)
            conn.close()
        for _ in range(FLOOD):
            conn = mux.connect_local(INTRUDER_USER)
            _send_regardless(conn, mux_register(secrets.token_bytes(REGISTRATION_SECRET_SIZE)))
            opened.append(conn)
        # On the port: connections that never say Hello, and some that say
        # half of one.
        for i in range(FLOOD):
            conn = mux.connect_tcp()
            if i % 4 == 0:
                _send_regardless(conn, hello(secrets.token_bytes(SESSION_ID_SIZE))[:7])
            opened.append(conn)
        assert mux.alive(), mux.log()
        # The session that was live stays live: it no longer goes through the
        # mux at all.
        _round_trip(terminal, "under-load")
        for conn in opened:
            conn.close()
        opened.clear()
        # And once the flood is gone, a new session gets in.
        other = _start(second, mux)
        try:
            other.expect(PROMPT)
            other.send(_typed("exit 0"))
            assert other.finish() == 0
        finally:
            other.kill()
        assert mux.alive(), mux.log()
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        for conn in opened:
            conn.close()
        terminal.kill()


@pytest.mark.parametrize("sig", [signal.SIGTERM, signal.SIGKILL], ids=["term", "kill"])
def test_a_mux_restart_does_not_end_a_session(binaries, installed_agent, two_hosts, mux,
                                              iptables, sig):
    zet = _zet(binaries, installed_agent, two_hosts[0])
    terminal = _start(zet, mux)
    try:
        terminal.expect(PROMPT)
        mux.wait_registered(_uid(TEST_USER))
        before = connection_to_agent(terminal.pid, zet.host.port)
        mux.stop(sig)
        # The connection was handed over: it does not notice the mux go.
        _round_trip(terminal, "mux-is-gone")
        assert connection_to_agent(terminal.pid, zet.host.port).inode == before.inode
        assert LOST_NOTICE not in terminal.buffer

        seen = len(mux.registrations())
        mux.start()
        # The agent registers again with the new mux, on its own.
        mux.wait_registered(_uid(TEST_USER), after=seen)
        _round_trip(terminal, "mux-is-back")
        # And a reconnect after a cut is routed by the new mux.
        _cut_and_reconnect(terminal, zet, iptables, mux.port)
        _round_trip(terminal, "routed-again")
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_without_a_mux_the_agent_listens_itself(binaries, installed_agent, two_hosts, muxd):
    zet = _zet(binaries, installed_agent, two_hosts[0])
    absent = Path("/run/zet-test-absent/mux.sock")
    assert not absent.exists()
    terminal = _start(zet, None, ZET_MUX_SOCKET=str(absent))
    try:
        terminal.expect(PROMPT)
        agent = parent_of(_shell_pid(terminal))
        held = sockets_of(agent, zet.host.user)
        listening = [s for s in held if s.state == TCP_LISTEN]
        assert len(listening) == 1, held
        assert connection_to_agent(terminal.pid, zet.host.port).remote_port == listening[0].local_port
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


def test_an_unknown_sid_looks_like_a_known_one(binaries, installed_agent, two_hosts, mux):
    """Through the mux, a Hello for a live session and a Hello for no session
    get the same answer, and the same end after a wrong Auth. Timing is not
    measured here."""
    zet = _zet(binaries, installed_agent, two_hosts[0])
    terminal = _start(zet, mux)
    try:
        terminal.expect(PROMPT)
        known = bytes.fromhex(mux.wait_registered(_uid(TEST_USER)))
        answers = []
        for sid in (known, secrets.token_bytes(SESSION_ID_SIZE)):
            with mux.connect_tcp() as conn:
                conn.sendall(hello(sid))
                challenge = read_frame(conn)
                assert challenge is not None, "no answer to Hello"
                epoch, body = challenge
                assert epoch == CLEARTEXT_EPOCH
                assert body[0] == TYPE_CHALLENGE, body[:1]
                conn.sendall(auth())
                rest = read_to_end(conn)
                answers.append((len(body), len(rest)))
        assert answers[0] == answers[1], answers
        # The live session did not notice either probe.
        _round_trip(terminal, "unbothered")
        terminal.send(_typed("exit 0"))
        assert terminal.finish() == 0
    finally:
        terminal.kill()


# ─── What /proc says about who holds a connection ────────────────────────────


def _pids() -> list[int]:
    return [int(name) for name in os.listdir("/proc") if name.isdigit()]


def _owner(pid: int) -> str:
    try:
        return pwd.getpwuid(os.stat(f"/proc/{pid}").st_uid).pw_name
    except (FileNotFoundError, KeyError):
        return "?"


def _holds(pid: int, local_port: int, remote_port: int) -> bool:
    """Whether `pid` holds the TCP socket from `local_port` to `remote_port`:
    the agent's side of the client's connection, seen from the agent."""
    owner = _owner(pid)
    try:
        held = sockets_of(pid, None if owner in ("root", "?") else owner)
    except (FileNotFoundError, ProcessLookupError):
        return False
    return any(s.local_port == local_port and s.remote_port == remote_port for s in held)
