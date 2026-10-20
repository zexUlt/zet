"""Fixtures of the systemd suite (design.md §16).

The Alpine container of the integration suite has no systemd, so there the
mux's socket activation is played by a launcher. This suite runs on a host
whose PID 1 is systemd — the GitHub Ubuntu runner, by the systemd-e2e
workflow — with the shipped units/zet-muxd.{socket,service} installed as
they are, zet, zet-agent and zet-muxd in /usr/local/bin, the `zet` user, and
zet-muxd.socket started. It needs root, and skips without all of that.

Fixtures:

    units      the installed units, the mux's main pid and its log
    two_hosts  one sshd on the host letting two test users in, a client
               config for each
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from e2e import PROFILE  # noqa: E402
from units import BIN_DIR, SECOND_USER, SOCKET_UNIT, Units  # noqa: E402
from sshhost import (CLEAN_BASHRC, TEST_USER, SshHost, authorize,  # noqa: E402
                     ensure_user, make_keys, start_sshd, stop_sshd,
                     write_client_config)


def pytest_collection_modifyitems(config, items):
    reason = None
    if not Path("/run/systemd/system").is_dir():
        reason = "PID 1 is not systemd: run through the systemd-e2e workflow"
    elif os.geteuid() != 0:
        reason = "the suite drives systemctl and starts sshd: it needs root"
    elif not (BIN_DIR / "zet-muxd").exists():
        reason = f"no zet-muxd in {BIN_DIR}: the workflow installs it"
    if reason:
        for item in items:
            item.add_marker(pytest.mark.skip(reason=reason))


@pytest.fixture(scope="session")
def units() -> Units:
    found = Units()
    if found.state(SOCKET_UNIT) != "active":
        pytest.fail(f"{SOCKET_UNIT} is not active: the workflow starts it\n{found.journal()}")
    return found


@pytest.fixture(scope="session")
def two_hosts(tmp_path_factory) -> tuple[SshHost, SshHost]:
    """One sshd on the host for two test users, and a client config each."""
    work = tmp_path_factory.mktemp("sshd-systemd")
    work.chmod(0o755)
    first = ensure_user(TEST_USER)
    second = ensure_user(SECOND_USER)
    keys = make_keys(work, first)
    authorize(second, (work / "client_key.pub").read_text())
    sshd, port = start_sshd(work, keys, Path(first.pw_dir), "127.0.0.1",
                            users=(TEST_USER, SECOND_USER))
    hosts = []
    for entry in (first, second):
        alias = f"zet-systemd-{entry.pw_name}"
        config = write_client_config(
            work / f"ssh_config.{entry.pw_name}", hostname="127.0.0.1", port=port,
            identity=keys.client, known_hosts=work / f"known_hosts.{entry.pw_name}",
            host_public_key=(work / "host_key.pub").read_text(),
            user=entry.pw_name, alias=alias)
        host = SshHost(config=config, alias=alias, user=entry.pw_name,
                       home=Path(entry.pw_dir), port=port)
        host.write_home_file(".bashrc", CLEAN_BASHRC)
        host.write_home_file(".bash_profile", PROFILE)
        hosts.append(host)
    try:
        yield hosts[0], hosts[1]
    finally:
        stop_sshd(sshd)
