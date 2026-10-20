"""The test host: a real sshd and an ordinary user to log in as (design.md §16).

Shared by the in-container suite (conftest.py), which runs sshd on 127.0.0.1
for itself, and by host mode (host.py), which runs the same sshd for a client
on the other side of a container boundary — the macOS e2e job. What differs
between the two is only where sshd listens and how the client sees it.
"""

from __future__ import annotations

import os
import pwd
import socket
import subprocess
from dataclasses import dataclass
from pathlib import Path

from harness import STEP_TIMEOUT_SECONDS, Timeout, read_until

SSHD = "/usr/sbin/sshd"

# The user the agent runs as: not root, with bash as its login shell so that
# .bashrc is read the way it is on a real host.
TEST_USER = "zettest"

# What `ssh -F <config>` knows the test host as.
HOST_ALIAS = "zet-test-host"

# Printed by the noisy .bashrc. Everything a careless rc file does to stdout
# at login: a banner (NOISE_BANNER), a lookalike of our own bootstrap block,
# bytes that are not text, and a line without a newline at the end.
NOISY_BASHRC = r"""
echo 'Welcome to the noisy host'
echo '-----BEGIN ZET BOOTSTRAP-----'
echo 'bm90IGEgcmVhbCBmcmFtZSwganVzdCBub2lzZQ=='
echo '-----END ZET BOOTSTRAP-----'
printf '\000\001\377\376 not text\r\n'
echo 'noise on stderr' >&2
printf 'no newline at the end'
"""

CLEAN_BASHRC = "# nothing\n"

# How many ports start_sshd tries before giving up.
SSHD_START_ATTEMPTS = 3


@dataclass
class SshHost:
    """A host reachable as `ssh -F config alias`, logged in as `user`."""

    config: Path
    alias: str
    user: str
    home: Path
    port: int

    def argv(self, *args: str) -> list[str]:
        """An ssh command line to this host."""
        return ["ssh", "-F", str(self.config), *args, self.alias]

    def run(self, command: str, *, input: bytes | None = None,
            timeout: float = STEP_TIMEOUT_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(self.argv("-T") + [command], input=input,
                              capture_output=True, timeout=timeout)

    def write_home_file(self, relative: str, content: str, mode: int = 0o644) -> None:
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        entry = pwd.getpwnam(self.user)
        os.chown(path, entry.pw_uid, entry.pw_gid)
        path.chmod(mode)


def _run(argv: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(argv, check=True, timeout=STEP_TIMEOUT_SECONDS, **kwargs)


def free_port(address: str = "127.0.0.1") -> int:
    # Free when probed, not necessarily when sshd binds it: another process
    # may take it in between, which is why start_sshd retries on a new one.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((address, 0))
        return probe.getsockname()[1]


def ensure_user(name: str = TEST_USER, shell: str = "/bin/bash") -> pwd.struct_passwd:
    try:
        return pwd.getpwnam(name)
    except KeyError:
        pass
    if os.path.exists("/etc/alpine-release"):
        _run(["adduser", "-D", "-s", shell, name])
    else:
        # The systemd suite runs on the Ubuntu runner itself, whose adduser
        # is a different program; useradd is the same on every glibc distro.
        _run(["useradd", "--create-home", "--shell", shell, name])
    # Both lock a new account with '!', and sshd refuses a locked account
    # even for a public key. '*' has no password and is not locked.
    _run(["chpasswd", "-e"], input=f"{name}:*\n".encode())
    return pwd.getpwnam(name)


def authorize(user: pwd.struct_passwd, public_key: str) -> None:
    """Lets `public_key` log in as `user`."""
    ssh_dir = Path(user.pw_dir) / ".ssh"
    ssh_dir.mkdir(mode=0o700, exist_ok=True)
    os.chown(ssh_dir, user.pw_uid, user.pw_gid)
    authorized = ssh_dir / "authorized_keys"
    authorized.write_text(public_key)
    os.chown(authorized, user.pw_uid, user.pw_gid)
    authorized.chmod(0o600)


@dataclass
class Keys:
    host: Path
    client: Path


def make_keys(work: Path, user: pwd.struct_passwd) -> Keys:
    """A fresh host key, and a client key the test user accepts."""
    keys = Keys(host=work / "host_key", client=work / "client_key")
    _run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(keys.host)])
    _run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(keys.client)])
    authorize(user, (work / "client_key.pub").read_text())
    return keys


def write_sshd_config(work: Path, keys: Keys, home: Path, listen: str,
                      port: int, users: tuple[str, ...] = (TEST_USER,)) -> Path:
    config = work / "sshd_config"
    config.write_text(f"""\
ListenAddress {listen}
Port {port}
HostKey {keys.host}
PidFile {work / "sshd.pid"}
AuthorizedKeysFile .ssh/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
AllowUsers {' '.join(users)}
LogLevel INFO
# Debian and Ubuntu build bash to read ~/.bashrc when sshd runs a command;
# Alpine's does not. BASH_ENV makes it, so the noisy rc file reaches the
# channel the way it would on the hosts zet meets in the wild.
SetEnv BASH_ENV={home / ".bashrc"}
# What the client hands the agent through `ssh -o SetEnv` (the e2e suite's
# timeouts, and the sanitizer options of an ASan build).
AcceptEnv ZET_* ASAN_OPTIONS UBSAN_OPTIONS
""")
    return config


def write_client_config(path: Path, *, hostname: str, port: int,
                        identity: Path, known_hosts: Path,
                        host_public_key: str,
                        known_hosts_as: Path | None = None,
                        user: str = TEST_USER, alias: str = HOST_ALIAS) -> Path:
    """An ssh_config for the client side, written at `path`, with
    known_hosts written at `known_hosts`. The address, `identity` and
    `known_hosts_as` are as the client sees them, which in host mode is not as
    the container does."""
    known_hosts.write_text(f"[{hostname}]:{port} {host_public_key}")
    seen = known_hosts_as if known_hosts_as is not None else known_hosts
    path.write_text(f"""\
Host {alias}
    HostName {hostname}
    Port {port}
    User {user}
    IdentityFile {identity}
    IdentitiesOnly yes
    UserKnownHostsFile {seen}
    StrictHostKeyChecking yes
    BatchMode yes
    LogLevel ERROR
""")
    return path


def start_sshd(work: Path, keys: Keys, home: Path, listen: str,
               port: int | None = None,
               users: tuple[str, ...] = (TEST_USER,)) -> tuple[subprocess.Popen, int]:
    """sshd in the foreground, listening, its log draining into
    work/sshd.log. Returns the sshd and the port. With no port given, picks
    a free one, and another if sshd finds it taken."""
    attempts = SSHD_START_ATTEMPTS if port is None else 1
    for attempt in range(attempts):
        chosen = port if port is not None else free_port(listen)
        config = write_sshd_config(work, keys, home, listen, chosen, users)
        # -D keeps it in the foreground, -e sends the log to stderr, which
        # is how the caller learns that it listens: no polling the port. A
        # taken port shows as sshd exiting before that line.
        sshd = subprocess.Popen([SSHD, "-D", "-e", "-f", str(config)],
                                stderr=subprocess.PIPE)
        try:
            read_until(sshd.stderr.fileno(), f"Server listening on {listen} port".encode())
            break
        except (Timeout, AssertionError):
            sshd.kill()
            sshd.wait()
            if attempt + 1 == attempts:
                raise
    # Read sshd's log for as long as it runs: every login writes to it, and a
    # pipe nobody drains would block sshd once its buffer filled. A process
    # rather than a thread: the suite forks ptys, and a forked threaded
    # process is how child deadlocks start.
    log = open(work / "sshd.log", "ab")
    drain = subprocess.Popen(["cat"], stdin=sshd.stderr, stdout=log)
    sshd.stderr.close()
    log.close()
    sshd.drain = drain  # type: ignore[attr-defined]
    return sshd, chosen


def stop_sshd(sshd: subprocess.Popen) -> None:
    sshd.terminate()
    for process in (sshd, getattr(sshd, "drain", None)):
        if process is None:
            continue
        try:
            process.wait(timeout=STEP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
