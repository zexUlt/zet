"""Fixtures of the integration suite (design.md §16).

The suite runs inside the Alpine container of tools/integration.sh, as root:
it starts a real sshd on 127.0.0.1 and logs in as an ordinary user, because
the agent must work without root and is tested as the user it will run as.
"""

from __future__ import annotations

import os
import pwd as _pwd
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

from e2e import PROFILE, Zet, install_agent
from harness import STEP_TIMEOUT_SECONDS

from sshhost import (CLEAN_BASHRC, HOST_ALIAS, NOISY_BASHRC, SSHD, TEST_USER,
                     SshHost, authorize, ensure_user, make_keys, start_sshd,
                     stop_sshd, write_client_config)


# Shell users created for the login-shell delivery tests (M5.5 §16).
# Each maps shell_name -> (username, shell_binary).
SHELL_USERS = {
    "ash":  ("zettest-ash",  "/bin/ash"),
    "bash": ("zettest-bash", "/bin/bash"),
    "zsh":  ("zettest-zsh",  "/usr/bin/zsh"),
    "fish": ("zettest-fish", "/usr/bin/fish"),
    "tcsh": ("zettest-tcsh", "/usr/bin/tcsh"),
}



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


@pytest.fixture(scope="session")
def ssh_host(tmp_path_factory) -> SshHost:
    """sshd on 127.0.0.1 with fresh keys, for the life of the test session."""
    work = tmp_path_factory.mktemp("sshd")
    user = ensure_user()
    home = Path(user.pw_dir)
    keys = make_keys(work, user)

    # Create shell users for the login-shell delivery tests and authorize the
    # same client key for them.  Shells that are not installed are silently
    # skipped; their tests will be skipped at collection time.
    client_pub = (work / "client_key.pub").read_text()
    shell_usernames: list[str] = []
    for _shell_name, (username, shell) in SHELL_USERS.items():
        if not Path(shell).exists():
            continue
        try:
            su = ensure_user(username, shell)
            authorize(su, client_pub)
            shell_usernames.append(username)
        except Exception:
            pass

    all_users = tuple([TEST_USER] + shell_usernames)
    sshd, port = start_sshd(work, keys, home, "127.0.0.1", users=all_users)
    config = write_client_config(
        work / "ssh_config", hostname="127.0.0.1", port=port,
        identity=keys.client, known_hosts=work / "known_hosts",
        host_public_key=(work / "host_key.pub").read_text())
    try:
        yield SshHost(config=config, alias=HOST_ALIAS, user=TEST_USER,
                      home=home, port=port, work_dir=work)
    finally:
        stop_sshd(sshd)


@pytest.fixture(scope="session")
def shell_ssh_hosts(ssh_host: SshHost, tmp_path_factory) -> dict[str, SshHost]:
    """One SshHost per login-shell variant, all backed by the session sshd.

    Keys absent from the dict mean the shell binary was not present at startup.
    """
    if ssh_host.work_dir is None:
        return {}
    work = tmp_path_factory.mktemp("shell_hosts")
    identity = ssh_host.work_dir / "client_key"
    host_pub = (ssh_host.work_dir / "host_key.pub").read_text()
    result: dict[str, SshHost] = {}
    for shell_name, (username, _shell) in SHELL_USERS.items():
        try:
            u = _pwd.getpwnam(username)
        except KeyError:
            continue
        config = write_client_config(
            work / f"ssh_config_{shell_name}",
            hostname="127.0.0.1", port=ssh_host.port,
            identity=identity,
            known_hosts=work / f"known_hosts_{shell_name}",
            host_public_key=host_pub,
            user=username,
            alias=f"zet-{shell_name}-host",
        )
        result[shell_name] = SshHost(
            config=config,
            alias=f"zet-{shell_name}-host",
            user=username,
            home=Path(u.pw_dir),
            port=ssh_host.port,
        )
    return result


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
def installed_agent(binaries: Binaries) -> Path:
    """zet-agent copied where the test user can run it (e2e.AGENT_DIR)."""
    return install_agent(binaries.agent)


@pytest.fixture
def zet(binaries: Binaries, installed_agent: Path, ssh_host: SshHost) -> Zet:
    """zet against the test host, with a login profile whose prompt the tests
    can wait for."""
    ssh_host.write_home_file(".bash_profile", PROFILE)
    return Zet(client=binaries.client, agent=installed_agent, host=ssh_host)


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
