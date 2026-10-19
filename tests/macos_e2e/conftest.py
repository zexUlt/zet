"""Fixtures of the macOS end-to-end suite (design.md §16).

The client runs here, natively; the host is the Alpine container that
tools/macos_e2e_host.sh starts, with sshd, the test user and the Linux
zet-agent. Everything this side does to the host goes through ssh as the
test user, or through `docker exec` for what needs root there (iptables).

Fixtures for the scenarios:

    remote_host   the host: ssh_config, alias, argv(), run(), files, exec()
    clean_bashrc  remote_host with a .bashrc that prints nothing
    noisy_bashrc  remote_host with the noisy one, put back afterwards
    macos_zet     the native zet; skips until it is built
    iptables      `docker exec` of iptables in the host; skips if unusable
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from harness import STEP_TIMEOUT_SECONDS  # noqa: E402
from sshhost import CLEAN_BASHRC, NOISY_BASHRC  # noqa: E402

# Where tools/macos_e2e_host.sh leaves its description of the host.
DESCRIPTION = Path(os.environ.get("ZET_E2E_HOST", ROOT / "build/macos-e2e/host.json"))

# Where the native build puts the client, unless ZET_MACOS_CLIENT says.
CLIENT_BUILD = ROOT / "build" / "dev"


@dataclass
class RemoteHost:
    """The container host, as the macOS side reaches it."""

    config: Path
    alias: str
    user: str
    home: str
    agent: str
    agent_ports: range
    container: str

    def argv(self, *args: str) -> list[str]:
        """An ssh command line to the host."""
        return ["ssh", "-F", str(self.config), *args, self.alias]

    def run(self, command: str, *, input: bytes | None = None,
            timeout: float = STEP_TIMEOUT_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(self.argv("-T") + [command], input=input,
                              capture_output=True, timeout=timeout)

    def write_home_file(self, relative: str, content: str) -> None:
        """Writes a file in the test user's home, as the test user."""
        path = shlex.quote(relative)
        result = self.run(f"mkdir -p \"$(dirname {path})\" && cat > {path}",
                          input=content.encode())
        assert result.returncode == 0, result.stderr

    def exec(self, argv: list[str], *, timeout: float = STEP_TIMEOUT_SECONDS
             ) -> subprocess.CompletedProcess:
        """Runs `argv` in the host container as root."""
        return subprocess.run(["docker", "exec", self.container, *argv],
                              capture_output=True, timeout=timeout)


@pytest.fixture(scope="session")
def remote_host() -> RemoteHost:
    if not DESCRIPTION.exists():
        pytest.skip(f"no {DESCRIPTION}: start the host with tools/macos_e2e_host.sh up")
    described = json.loads(DESCRIPTION.read_text())
    first, last = (int(port) for port in described["agent_ports"].split("-"))
    return RemoteHost(config=Path(described["ssh_config"]),
                      alias=described["alias"], user=described["user"],
                      home=described["home"], agent=described["agent"],
                      agent_ports=range(first, last + 1),
                      container=described["container"])


@pytest.fixture
def clean_bashrc(remote_host: RemoteHost) -> RemoteHost:
    remote_host.write_home_file(".bashrc", CLEAN_BASHRC)
    return remote_host


@pytest.fixture
def noisy_bashrc(remote_host: RemoteHost):
    remote_host.write_home_file(".bashrc", NOISY_BASHRC)
    try:
        yield remote_host
    finally:
        remote_host.write_home_file(".bashrc", CLEAN_BASHRC)


@pytest.fixture(scope="session")
def macos_zet() -> Path:
    """The native client. Skips until the macOS port builds one."""
    explicit = os.environ.get("ZET_MACOS_CLIENT")
    candidates = [Path(explicit)] if explicit else [
        CLIENT_BUILD / "src" / "client" / "zet", CLIENT_BUILD / "zet"]
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    pytest.skip(f"no native zet at {', '.join(map(str, candidates))}: "
                "the macOS client is not built yet")


@pytest.fixture(scope="session")
def iptables(remote_host: RemoteHost) -> RemoteHost:
    """The host, once iptables in it is known to work. Rules for traffic from
    this side go on eth0, which is where docker delivers it, not on lo."""
    probe = remote_host.exec(["iptables", "-L", "INPUT", "-n"])
    if probe.returncode != 0:
        pytest.skip(f"iptables unusable in the host: {probe.stderr.decode(errors='replace').strip()}")
    return remote_host
