"""Driving zet-muxd for the M5 suite: the CLI contract it is held to, a
launcher that hands it its port the way systemd would, and the few frames
the attack scenarios write by hand.

The contract (design.md §6, §17 M5):

    zet-muxd [--port N]

runs as a dedicated unprivileged user, `zet` here. Its TCP listener comes
from systemd socket activation: LISTEN_FDS=1 and LISTEN_PID=<its pid>, the
socket bound and listening on descriptor 3 (sd_listen_fds(3)). Without them
it binds `--port N` itself. It never needs root, and the suite never gives
it root.

    ZET_MUX_SOCKET   path  where it binds the agents' unix socket;
                           default /run/zet/mux.sock. The directory is the
                           mux user's (it binds there) and 0755; the socket
                           is the mux user's and connectable by every user,
                           since every user's agent registers on it
    ZET_MUX_LOG      path  where it logs; nothing if unset

Its log has one line per accepted registration, which is how the suite
learns a live session's sid (public: it rides in every Hello, §5):

    registered sid=<32 lowercase hex> uid=<decimal>

zet-agent --bootstrap uses the mux when the socket at ZET_MUX_SOCKET (or the
default path) exists: it registers its session, `MuxRegister { R[32] }`, and
announces the mux's port in BootstrapAccept. Without the socket it binds its
own port, as in M4. ZET_MUX_SOCKET reaches the agent like the other ZET_*
settings, through `--ssh-arg -oSetEnv=...` (e2e.py). The client is
unchanged.

On the agents' socket a message is a cleartext frame of the usual format
(§7): length:u32be, epoch:u8 = 0, then the body, whose first byte is the
message type. MUX_REGISTER below is the type the suite assumes for
MuxRegister; the attack scenarios send it with an R the attacker made up.
"""

from __future__ import annotations

import os
import pwd
import secrets
import shutil
import signal
import socket
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from harness import STEP_TIMEOUT_SECONDS, wait_gone, wait_until
from sshhost import ensure_user

# The unprivileged user the mux runs as (§6).
MUX_USER = "zet"

# A local user with no business with anyone's session: the attacker of the
# M5 criterion. Logs in nowhere; the suite runs code as it directly.
INTRUDER_USER = "zetintruder"

# Where the suite puts the mux's directory: not /run/zet, so that a run
# cannot disturb a mux the machine may have of its own.
MUX_DIR = Path("/run/zet-test")

SD_LISTEN_FDS_START = 3

# What systemd does between fork and exec for a socket-activated service:
# the listener on fd 3, LISTEN_PID the service's own pid.
LAUNCHER = """
import os, sys
fd = int(sys.argv[1])
if fd != 3:
    os.dup2(fd, 3)
    os.close(fd)
os.set_inheritable(3, True)
os.environ["LISTEN_PID"] = str(os.getpid())
os.execv(sys.argv[2], sys.argv[2:])
"""

# The frame and Hello layout of §7, as far as the hand-written frames need it.
HEADER = struct.Struct(">IB")
CLEARTEXT_EPOCH = 0
TYPE_HELLO = 0x01
TYPE_CHALLENGE = 0x02
TYPE_AUTH = 0x03
HELLO_MAGIC = b"ZET1"
PROTOCOL_VERSION = 1
SESSION_ID_SIZE = 16
HANDSHAKE_NONCE_SIZE = 16
AUTH_TAG_SIZE = 32
REGISTRATION_SECRET_SIZE = 32

# The message type the suite assumes for MuxRegister on the agents' socket.
# 0x20 opens the range §7 leaves free; zet-muxd and zet-agent must agree.
MUX_REGISTER = 0x20


def frame(body: bytes) -> bytes:
    return HEADER.pack(len(body), CLEARTEXT_EPOCH) + body


def hello(sid: bytes, nonce: bytes | None = None) -> bytes:
    """A Hello frame for `sid`, no options."""
    assert len(sid) == SESSION_ID_SIZE
    nonce = nonce if nonce is not None else secrets.token_bytes(HANDSHAKE_NONCE_SIZE)
    return frame(bytes([TYPE_HELLO]) + HELLO_MAGIC
                 + struct.pack(">HH", PROTOCOL_VERSION, PROTOCOL_VERSION)
                 + sid + nonce)


def auth(tag: bytes | None = None) -> bytes:
    tag = tag if tag is not None else secrets.token_bytes(AUTH_TAG_SIZE)
    return frame(bytes([TYPE_AUTH]) + tag)


def mux_register(registration_secret: bytes) -> bytes:
    assert len(registration_secret) == REGISTRATION_SECRET_SIZE
    return frame(bytes([MUX_REGISTER]) + registration_secret)


def read_frame(sock: socket.socket, timeout: float = STEP_TIMEOUT_SECONDS) -> tuple[int, bytes] | None:
    """One frame's epoch and body, or None at the end of the stream."""
    sock.settimeout(timeout)
    header = _read_exactly(sock, HEADER.size)
    if header is None:
        return None
    length, epoch = HEADER.unpack(header)
    body = _read_exactly(sock, length)
    if body is None:
        raise AssertionError(f"the stream ended inside a frame of {length} bytes")
    return epoch, body


def read_to_end(sock: socket.socket, timeout: float = STEP_TIMEOUT_SECONDS) -> bytes:
    """Whatever comes until the peer closes, within `timeout`."""
    sock.settimeout(timeout)
    received = bytearray()
    while True:
        try:
            chunk = sock.recv(4096)
        except ConnectionResetError:
            break
        if not chunk:
            break
        received += chunk
    return bytes(received)


def _read_exactly(sock: socket.socket, count: int) -> bytes | None:
    data = bytearray()
    while len(data) < count:
        try:
            chunk = sock.recv(count - len(data))
        except ConnectionResetError:
            chunk = b""
        if not chunk:
            if data:
                raise AssertionError(f"the stream ended after {len(data)} of {count} bytes")
            return None
        data += chunk
    return bytes(data)


def as_user(name: str):
    """A preexec_fn that becomes `name`, groups included, before exec."""
    entry = pwd.getpwnam(name)

    def drop() -> None:
        os.setgroups([])
        os.setgid(entry.pw_gid)
        os.setuid(entry.pw_uid)

    return drop


@dataclass
class Mux:
    """A running zet-muxd and what the suite needs to know about it."""

    binary: Path
    socket_path: Path
    log_path: Path
    port: int
    listener: socket.socket
    process: subprocess.Popen | None = None

    def start(self) -> None:
        """Starts the mux as MUX_USER with the listener on fd 3, the way
        systemd's socket activation hands it over. LISTEN_PID has to be the
        mux's own pid, which only the process itself knows before exec: a
        small Python launcher moves the listener to fd 3, sets LISTEN_PID to
        its own pid and execs, keeping the pid. (Popen's own fd juggling
        closes everything not in pass_fds after preexec_fn runs, so the move
        cannot happen there.)"""
        assert self.process is None or self.process.poll() is not None
        fd = self.listener.fileno()
        env = {
            "PATH": "/usr/bin:/bin",
            "LISTEN_FDS": "1",
            "ZET_MUX_SOCKET": str(self.socket_path),
            "ZET_MUX_LOG": str(self.log_path),
            "ASAN_OPTIONS": "detect_leaks=0",
        }
        self.process = subprocess.Popen(
            ["python3", "-c", LAUNCHER, str(fd), str(self.binary)],
            env=env, preexec_fn=as_user(MUX_USER), pass_fds=(fd,),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Ready is the agents' socket taking a connection. Not the path
        # existing: after a SIGKILL the old one is still there. No descriptor
        # says when another process starts listening, hence the poll.
        wait_until(lambda: self._accepts() or self.process.poll() is not None,
                   "zet-muxd to listen on its socket")
        if self.process.poll() is not None:
            raise AssertionError(f"zet-muxd exited with {self.process.returncode}: {self.log()!r}")

    def _accepts(self) -> bool:
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.connect(str(self.socket_path))
            return True
        except OSError:
            return False
        finally:
            probe.close()

    def stop(self, sig: int = signal.SIGTERM) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.send_signal(sig)
        try:
            self.process.wait(timeout=STEP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError(f"zet-muxd ignored signal {sig}") from None

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def log(self) -> str:
        try:
            return self.log_path.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def registrations(self) -> list[tuple[str, int]]:
        """(sid, uid) of every registration the log records, oldest first."""
        found = []
        for line in self.log().splitlines():
            if "registered sid=" not in line:
                continue
            fields = dict(part.split("=", 1) for part in line.split() if "=" in part)
            found.append((fields["sid"], int(fields["uid"])))
        return found

    def wait_registered(self, uid: int, after: int = 0) -> str:
        """The sid of the first registration by `uid` past the first `after`
        lines the log had. The log is a file another process appends to: no
        event for that, hence the poll."""
        def found() -> list[str]:
            return [sid for sid, owner in self.registrations()[after:] if owner == uid]
        wait_until(lambda: bool(found()), f"a registration by uid {uid}")
        return found()[0]

    def connect_local(self, user: str) -> socket.socket:
        """A connection to the agents' socket made as `user`: a child process
        becomes the user, connects, and hands the descriptor back over a
        socketpair, so that SO_PEERCRED at the mux says `user`."""
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        pid = os.fork()
        if pid == 0:
            try:
                parent.close()
                as_user(user)()
                conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                conn.connect(str(self.socket_path))
                socket.send_fds(child, [b"x"], [conn.fileno()])
                os._exit(0)
            except BaseException:
                os._exit(1)
        child.close()
        try:
            _, fds, _, _ = socket.recv_fds(parent, 1, 1)
        finally:
            parent.close()
            _, status = os.waitpid(pid, 0)
        if os.waitstatus_to_exitcode(status) != 0 or not fds:
            raise AssertionError(f"{user} could not connect to {self.socket_path}")
        return socket.socket(fileno=fds[0])

    def connect_tcp(self) -> socket.socket:
        return socket.create_connection(("127.0.0.1", self.port), timeout=STEP_TIMEOUT_SECONDS)


def find_muxd() -> Path | None:
    build = Path(os.environ.get("ZET_BUILD_DIR", "build/musl-dev"))
    for directory, subdirs, files in os.walk(build):
        subdirs[:] = [d for d in subdirs if d != "_deps" and not d.startswith(".")]
        if "zet-muxd" in files:
            path = Path(directory) / "zet-muxd"
            if os.access(path, os.X_OK):
                return path
    return None


def install_muxd(built: Path, directory: Path) -> Path:
    """Copies zet-muxd where its user can run it."""
    directory.mkdir(mode=0o755, parents=True, exist_ok=True)
    target = directory / "zet-muxd"
    shutil.copyfile(built, target)
    target.chmod(0o755)
    return target


def prepare_mux_dir() -> Path:
    """MUX_DIR, owned by the mux user and 0755: where it binds its socket,
    which nobody else may replace."""
    entry = ensure_user(MUX_USER, shell="/sbin/nologin")
    if MUX_DIR.exists():
        shutil.rmtree(MUX_DIR)
    MUX_DIR.mkdir(mode=0o755)
    os.chown(MUX_DIR, entry.pw_uid, entry.pw_gid)
    MUX_DIR.chmod(0o755)
    return MUX_DIR


def listener() -> socket.socket:
    """The TCP listener systemd would bind for the mux: 127.0.0.1, a free
    port, inheritable so that it survives into the mux."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    sock.listen(128)
    return sock


def wait_mux_gone(mux: Mux) -> None:
    if mux.process is not None:
        wait_gone(mux.process.pid)
