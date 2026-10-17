"""Fixtures of the integration suite (design.md §16).

The suite runs inside the Alpine container of tools/integration.sh, as root:
it starts a real sshd on 127.0.0.1 and logs in as an ordinary user, because
the agent must work without root and is tested as the user it will run as.
"""

from __future__ import annotations

import os
import pwd
import shutil
import socket
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

from harness import NOISE_BANNER, STEP_TIMEOUT_SECONDS, Timeout, read_until

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


def pytest_collection_modifyitems(config, items):
    """Outside the container there is no sshd to start and no root to start it
    with; say so once per test instead of failing on the first fixture."""
    reason = None
    if not os.path.exists(SSHD):
        reason = f"no {SSHD}: run the suite through tools/integration.sh"
    elif os.geteuid() != 0:
        reason = "the suite starts sshd and needs root: run it through tools/integration.sh"
    if reason:
        for item in items:
            item.add_marker(pytest.mark.skip(reason=reason))


def _run(argv: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(argv, check=True, timeout=STEP_TIMEOUT_SECONDS, **kwargs)


# How many ports the fixture tries before giving up on starting sshd.
SSHD_START_ATTEMPTS = 3


def _free_port() -> int:
    # Free when probed, not necessarily when sshd binds it: another process
    # may take it in between, which is why the fixture retries on a new one.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


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


def _ensure_user() -> pwd.struct_passwd:
    try:
        return pwd.getpwnam(TEST_USER)
    except KeyError:
        pass
    _run(["adduser", "-D", "-s", "/bin/bash", TEST_USER])
    # adduser -D locks the account with '!', and sshd refuses a locked
    # account even for a public key. '*' has no password and is not locked.
    _run(["chpasswd", "-e"], input=f"{TEST_USER}:*\n".encode())
    return pwd.getpwnam(TEST_USER)


@pytest.fixture(scope="session")
def ssh_host(tmp_path_factory) -> SshHost:
    """sshd on 127.0.0.1 with fresh keys, for the life of the test session."""
    work = tmp_path_factory.mktemp("sshd")
    user = _ensure_user()
    home = Path(user.pw_dir)

    host_key = work / "host_key"
    client_key = work / "client_key"
    _run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(host_key)])
    _run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(client_key)])

    ssh_dir = home / ".ssh"
    ssh_dir.mkdir(mode=0o700, exist_ok=True)
    os.chown(ssh_dir, user.pw_uid, user.pw_gid)
    authorized = ssh_dir / "authorized_keys"
    authorized.write_text((work / "client_key.pub").read_text())
    os.chown(authorized, user.pw_uid, user.pw_gid)
    authorized.chmod(0o600)

    def write_configs(port: int) -> tuple[Path, Path]:
        sshd_config = work / "sshd_config"
        sshd_config.write_text(f"""\
ListenAddress 127.0.0.1
Port {port}
HostKey {host_key}
PidFile {work / "sshd.pid"}
AuthorizedKeysFile .ssh/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
AllowUsers {TEST_USER}
LogLevel INFO
# Debian and Ubuntu build bash to read ~/.bashrc when sshd runs a command;
# Alpine's does not. BASH_ENV makes it, so the noisy rc file reaches the
# channel the way it would on the hosts zet meets in the wild.
SetEnv BASH_ENV={home / ".bashrc"}
""")

        known_hosts = work / "known_hosts"
        known_hosts.write_text(f"[127.0.0.1]:{port} {(work / 'host_key.pub').read_text()}")
        client_config = work / "ssh_config"
        client_config.write_text(f"""\
Host {HOST_ALIAS}
    HostName 127.0.0.1
    Port {port}
    User {TEST_USER}
    IdentityFile {client_key}
    IdentitiesOnly yes
    UserKnownHostsFile {known_hosts}
    StrictHostKeyChecking yes
    BatchMode yes
    LogLevel ERROR
""")

        return sshd_config, client_config

    # -D keeps it in the foreground, -e sends the log to stderr, which is how
    # the fixture learns that it listens: no polling the port. A taken port
    # shows as sshd exiting before that line, and gets one more try.
    for attempt in range(SSHD_START_ATTEMPTS):
        port = _free_port()
        sshd_config, client_config = write_configs(port)
        sshd = subprocess.Popen([SSHD, "-D", "-e", "-f", str(sshd_config)],
                                stderr=subprocess.PIPE)
        try:
            read_until(sshd.stderr.fileno(), b"Server listening on 127.0.0.1 port")
            break
        except (Timeout, AssertionError):
            sshd.kill()
            sshd.wait()
            if attempt + 1 == SSHD_START_ATTEMPTS:
                raise

    # Read sshd's log for as long as it runs: every login writes to it, and a
    # pipe nobody drains would block sshd once its buffer filled. A process
    # rather than a thread: the suite forks ptys, and a forked threaded
    # process is how child deadlocks start.
    log = open(work / "sshd.log", "ab")
    drain = subprocess.Popen(["cat"], stdin=sshd.stderr, stdout=log)
    sshd.stderr.close()
    log.close()
    try:
        yield SshHost(config=client_config, alias=HOST_ALIAS, user=TEST_USER,
                      home=home, port=port)
    finally:
        sshd.terminate()
        for process in (sshd, drain):
            try:
                process.wait(timeout=STEP_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


@pytest.fixture
def clean_bashrc(ssh_host: SshHost) -> SshHost:
    """A .bashrc that prints nothing."""
    ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)
    return ssh_host


@pytest.fixture
def noisy_bashrc(ssh_host: SshHost):
    """A .bashrc that prints NOISY_BASHRC at every login, put back to clean
    afterwards so that the next test starts quiet."""
    ssh_host.write_home_file(".bashrc", NOISY_BASHRC)
    try:
        yield ssh_host
    finally:
        ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)


@dataclass
class Binaries:
    client: Path
    agent: Path


@pytest.fixture(scope="session")
def binaries() -> Binaries:
    """zet and zet-agent from the build tools/integration.sh made. Skips until
    they exist: the suite lands before the binaries do."""
    build = Path(os.environ.get("ZET_BUILD_DIR", "build/musl-dev"))
    found: dict[str, Path] = {}
    for directory, subdirs, files in os.walk(build):
        subdirs[:] = [d for d in subdirs if d != "_deps" and not d.startswith(".")]
        for name in ("zet", "zet-agent"):
            if name in files and name not in found:
                path = Path(directory) / name
                if os.access(path, os.X_OK):
                    found[name] = path
    missing = [name for name in ("zet", "zet-agent") if name not in found]
    if missing:
        pytest.skip(f"{' and '.join(missing)} not built under {build} yet")
    return Binaries(client=found["zet"], agent=found["zet-agent"])


@pytest.fixture(scope="session")
def iptables() -> str:
    """The iptables binary, if the container may use it. Skips with what the
    kernel said otherwise: NET_ADMIN is the runner's to grant."""
    tool = shutil.which("iptables")
    if tool is None:
        pytest.skip("iptables is not installed")
    probe = subprocess.run([tool, "-L", "INPUT", "-n"], capture_output=True,
                           timeout=STEP_TIMEOUT_SECONDS)
    if probe.returncode != 0:
        pytest.skip(f"iptables unusable here: {probe.stderr.decode(errors='replace').strip()}")
    return tool
