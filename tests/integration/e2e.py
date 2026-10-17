"""Driving zet end to end: the CLI contract the M4 suite holds the binaries
to, a terminal that keeps what it has read, and what /proc says about the
processes and sockets involved.

The contract (design.md §4, §9, §17):

    zet [--agent PATH] [--ssh-arg ARG]... HOST

runs `ssh -T <ssh-args> HOST <PATH, shell-quoted> --bootstrap`, then connects
over TCP to the host name `ssh -G <ssh-args> HOST` reports and the port the
agent announced in its BootstrapAccept. It puts the local terminal in raw
mode, puts it back when it ends, and exits with the remote shell's exit
code, or 128 + the signal that ended it.

    zet-agent --bootstrap

is what the client starts through ssh. It answers the bootstrap, detaches
from the ssh session and runs the user's login shell on a pty, for as long as
the session lives.

Both read their timeouts from the environment, which is how the suite makes
a cut or an orphan resolve in seconds instead of minutes:

    ZET_KEEPALIVE_INTERVAL    ms   both: send a Ping after this much quiet
    ZET_PEER_SILENCE_TIMEOUT  ms   both: drop a connection heard nothing from
    ZET_RECONNECT_MAX         ms   client: the ceiling of the reconnect backoff
    ZET_ORPHAN_TIMEOUT        s    agent: reap a session left without a client
    ZET_AGENT_LOG             path agent: where to log once detached

The client gets them from its own environment; the agent gets them through
`--ssh-arg -oSetEnv=...`, which sshd lets through with `AcceptEnv ZET_*`.
"""

from __future__ import annotations

import errno
import os
import select
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from harness import READ_CHUNK, STEP_TIMEOUT_SECONDS, PtyProcess, Timeout

# Where the agent binary is installed for the test user. A space on purpose:
# the client has to quote the path for the remote shell (§4), and a path that
# needs no quoting would not show whether it does.
AGENT_DIR = Path("/opt/zet test")

# The prompt the e2e .bash_profile sets. The user name comes from \u, so the
# text the test waits for never appears in a command it types.
PROFILE = "PS1='\\u@zet-prompt\\$ '\nexport PS1\n"
PROMPT = b"@zet-prompt$ "

# Short enough that a cut resolves within a test, long enough not to drop a
# connection that is merely busy on a loaded CI runner.
KEEPALIVE_INTERVAL_MS = 300
PEER_SILENCE_TIMEOUT_MS = 2000
RECONNECT_MAX_MS = 500

# Agents of tests that end abnormally go away on their own, well before the
# job ends, rather than after the default 30 minutes.
DEFAULT_ORPHAN_TIMEOUT_S = 20

# LeakSanitizer cannot read the main thread's stack under musl and reports
# false leaks (design.md §16, the same reason the fuzzers run without it).
SANITIZER_OPTIONS = {"ASAN_OPTIONS": "detect_leaks=0"}

TCP_ESTABLISHED = 0x01
TCP_LISTEN = 0x0A


@dataclass
class Zet:
    """The client and the installed agent, for one ssh host."""

    client: Path
    agent: Path
    host: object  # conftest.SshHost

    def argv(self, agent_env: dict[str, str]) -> list[str]:
        settings = " ".join(f"{name}={value}" for name, value in agent_env.items())
        return [str(self.client), "--agent", str(self.agent),
                "--ssh-arg", "-F", "--ssh-arg", str(self.host.config),
                "--ssh-arg", f"-oSetEnv={settings}",
                self.host.alias]

    def env(self) -> dict[str, str]:
        env = dict(os.environ)
        env.update(SANITIZER_OPTIONS)
        env.update({
            "ZET_KEEPALIVE_INTERVAL": str(KEEPALIVE_INTERVAL_MS),
            "ZET_PEER_SILENCE_TIMEOUT": str(PEER_SILENCE_TIMEOUT_MS),
            "ZET_RECONNECT_MAX": str(RECONNECT_MAX_MS),
            "TERM": "xterm",
        })
        return env

    def agent_env(self, **overrides: str) -> dict[str, str]:
        env = dict(SANITIZER_OPTIONS)
        env.update({
            "ZET_KEEPALIVE_INTERVAL": str(KEEPALIVE_INTERVAL_MS),
            "ZET_PEER_SILENCE_TIMEOUT": str(PEER_SILENCE_TIMEOUT_MS),
            "ZET_ORPHAN_TIMEOUT": str(DEFAULT_ORPHAN_TIMEOUT_S),
        })
        env.update(overrides)
        return env

    def start(self, **agent_env: str) -> "Terminal":
        """zet on a fresh pty of its own, as a user would run it."""
        return Terminal(PtyProcess(self.argv(self.agent_env(**agent_env)), env=self.env()))


def install_agent(built: Path) -> Path:
    """Copies zet-agent where the test user can run it."""
    AGENT_DIR.mkdir(mode=0o755, parents=True, exist_ok=True)
    AGENT_DIR.chmod(0o755)
    target = AGENT_DIR / "zet-agent"
    shutil.copyfile(built, target)
    target.chmod(0o755)
    return target


@dataclass
class Terminal:
    """A pty with what came out of it so far. Unlike harness.read_until, what
    was read past one needle is kept for the next."""

    process: PtyProcess
    buffer: bytearray = field(default_factory=bytearray)
    consumed: int = 0
    ended: bool = False

    @property
    def pid(self) -> int:
        return self.process.pid

    def send(self, data: bytes) -> None:
        self.process.send(data)

    def read_more(self, timeout: float = STEP_TIMEOUT_SECONDS) -> int:
        """Reads what is there, waiting up to `timeout` for something."""
        if self.ended:
            raise AssertionError(f"the terminal has ended: {self.tail()!r}")
        ready, _, _ = select.select([self.process.master], [], [], timeout)
        if not ready:
            raise Timeout(f"nothing on the terminal for {timeout}s: {self.tail()!r}")
        try:
            chunk = os.read(self.process.master, READ_CHUNK)
        except OSError as error:
            # EIO is how a pty master says its slave is gone; anything else is
            # a fault of the test, not the end of a session.
            if error.errno != errno.EIO:
                raise
            chunk = b""
        if not chunk:
            self.ended = True
            return 0
        self.buffer += chunk
        return len(chunk)

    def expect(self, needle: bytes, timeout: float = STEP_TIMEOUT_SECONDS) -> bytes:
        """Everything up to and including the next `needle`."""
        while True:
            at = self.buffer.find(needle, self.consumed)
            if at >= 0:
                out = bytes(self.buffer[self.consumed:at + len(needle)])
                self.consumed = at + len(needle)
                return out
            if self.read_more(timeout) == 0:
                raise AssertionError(f"the terminal ended before {needle!r}: {self.tail()!r}")

    def tail(self, length: int = 400) -> bytes:
        return bytes(self.buffer[-length:])

    def finish(self, timeout: float = STEP_TIMEOUT_SECONDS) -> int:
        """Reads to the end and returns zet's exit code."""
        while not self.ended:
            self.read_more(timeout)
        return self.process.finish(timeout).status

    def kill(self) -> None:
        self.process.kill()


# What /proc says. There is no event for "another process closed a socket",
# so the tests that need it look here, through harness.wait_until.

@dataclass(frozen=True)
class TcpSocket:
    inode: int
    local_port: int
    remote_port: int
    state: int


def socket_inodes(pid: int) -> set[int]:
    inodes = set()
    try:
        names = os.listdir(f"/proc/{pid}/fd")
    except FileNotFoundError:
        return inodes
    for name in names:
        try:
            link = os.readlink(f"/proc/{pid}/fd/{name}")
        except OSError:
            continue
        if link.startswith("socket:["):
            inodes.add(int(link[len("socket:["):-1]))
    return inodes


def tcp_sockets() -> list[TcpSocket]:
    found = []
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = Path(table).read_text().splitlines()[1:]
        except FileNotFoundError:
            continue
        for line in lines:
            fields = line.split()
            local, remote, state, inode = fields[1], fields[2], fields[3], fields[9]
            found.append(TcpSocket(inode=int(inode),
                                   local_port=int(local.rsplit(":", 1)[1], 16),
                                   remote_port=int(remote.rsplit(":", 1)[1], 16),
                                   state=int(state, 16)))
    return found


def sockets_of(pid: int) -> list[TcpSocket]:
    inodes = socket_inodes(pid)
    return [s for s in tcp_sockets() if s.inode in inodes]


def connection_to_agent(client_pid: int, ssh_port: int) -> TcpSocket:
    """The client's established connection to its agent."""
    live = [s for s in sockets_of(client_pid)
            if s.state == TCP_ESTABLISHED and s.remote_port != ssh_port]
    if len(live) != 1:
        raise AssertionError(f"expected one connection from zet to its agent, found {live}")
    return live[0]


def parent_of(pid: int) -> int:
    stat = Path(f"/proc/{pid}/stat").read_text()
    # The command name may hold spaces and parentheses; the fields after it
    # do not.
    return int(stat[stat.rindex(")") + 2:].split()[1])


def command_of(pid: int) -> str:
    return Path(f"/proc/{pid}/comm").read_text().strip()


def real_uid_of(pid: int) -> int:
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("Uid:"):
            return int(line.split()[1])
    raise AssertionError(f"no Uid line for pid {pid}")
