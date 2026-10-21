"""Delivery mode from the macOS client (design.md §16, §17 M5.5 criterion).

The macOS client delivers zet-agent to a Linux host that has no pre-installed
agent, over the same ssh -T session used for bootstrap.  The remote host
container starts without any agent; ZET_AGENT_DIR on the client side names the
directory that contains zet-agent-x86_64 (or the arch the host reports).

Scenarios:
  - A host with no agent gets the agent delivered and a session connects.
  - A second connect with the same binary copies nothing (inode and mtime
    stay the same).
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from conftest import RemoteHost  # noqa: E402
from e2e import PROFILE, PROMPT  # noqa: E402
from harness import STEP_TIMEOUT_SECONDS, PtyProcess  # noqa: E402
from sshhost import CLEAN_BASHRC  # noqa: E402

# The cache subdirectory the installer creates on the remote host.
ZET_CACHE_SUBDIR = ".cache/zet"

KEEPALIVE_INTERVAL_MS = 300
PEER_SILENCE_TIMEOUT_MS = 2000
RECONNECT_MAX_MS = 500
DEFAULT_ORPHAN_TIMEOUT_S = 20


def _delivery_argv(client: Path, host: RemoteHost) -> list[str]:
    """zet argv for delivery mode: no --agent, ZET_AGENT_DIR from environment."""
    settings = " ".join(f"{name}={value}" for name, value in {
        "ZET_KEEPALIVE_INTERVAL": KEEPALIVE_INTERVAL_MS,
        "ZET_PEER_SILENCE_TIMEOUT": PEER_SILENCE_TIMEOUT_MS,
        "ZET_ORPHAN_TIMEOUT": DEFAULT_ORPHAN_TIMEOUT_S,
        "ASAN_OPTIONS": "detect_leaks=0",
    }.items())
    return [str(client),
            "--ssh-arg", "-F", "--ssh-arg", str(host.config),
            "--ssh-arg", f"-oSetEnv={settings}", host.alias]


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["ZET_KEEPALIVE_INTERVAL"] = str(KEEPALIVE_INTERVAL_MS)
    env["ZET_PEER_SILENCE_TIMEOUT"] = str(PEER_SILENCE_TIMEOUT_MS)
    env["ZET_RECONNECT_MAX"] = str(RECONNECT_MAX_MS)
    env["TERM"] = "xterm"
    return env


@pytest.fixture
def clean_cache(remote_host: RemoteHost) -> RemoteHost:
    """Reset the remote user's .bashrc and clear the zet agent cache."""
    remote_host.write_home_file(".bashrc", CLEAN_BASHRC)
    remote_host.write_home_file(".bash_profile", PROFILE)
    remote_host.run("rm -rf ~/.cache/zet")
    yield remote_host


def test_delivery_installs_agent_and_session_works(macos_zet, clean_cache, agent_dir):
    """A Linux host with no pre-installed agent gets the agent delivered and a
    session opens.  After the connect the agent must be executable under
    ~/.cache/zet/<hash>/zet-agent on the remote host."""
    argv = _delivery_argv(macos_zet, clean_cache)
    terminal = PtyProcess(argv, env=_env())
    try:
        terminal.expect(PROMPT)
        terminal.send(b"echo delivery-ok\r")
        terminal.expect(b"delivery-ok")
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()

    # After a successful session the agent must be in the cache.
    result = clean_cache.run(
        "find ~/.cache/zet -name zet-agent -perm -u+x 2>/dev/null | wc -l",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    count = result.stdout.decode(errors="replace").strip()
    assert count == "1", (
        f"expected exactly one cached zet-agent after delivery, got {count!r}"
    )

    # The delivered binary must match the host's architecture.
    arch_result = clean_cache.run(
        r"""
set -e
arch=$(uname -m)
agent=$(find ~/.cache/zet -name zet-agent -perm -u+x | head -1)
elf_byte=$(od -An -tx1 -j18 -N1 "$agent" | tr -d ' \n')
case "$arch" in
    x86_64)  expect=3e ;;
    aarch64) expect=b7 ;;
    *)       echo "unknown arch $arch"; exit 1 ;;
esac
if [ "$elf_byte" != "$expect" ]; then
    echo "arch-mismatch: uname=$arch expect_elf=$expect got_elf=$elf_byte"
    exit 1
fi
echo "arch-ok"
""",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert arch_result.returncode == 0, (
        f"architecture check failed: {arch_result.stdout.decode(errors='replace').strip()}"
    )


def test_second_connect_copies_nothing(macos_zet, clean_cache, agent_dir):
    """A second connect with the same binary does not retransmit.

    The client replies ZET-HAVE on the second connect.  We check this
    indirectly: if the binary were rewritten its inode or mtime would change.
    Both staying equal proves no bytes were sent.
    """
    argv = _delivery_argv(macos_zet, clean_cache)

    def _connect() -> None:
        terminal = PtyProcess(argv, env=_env())
        try:
            terminal.expect(PROMPT)
            terminal.send(b"exit 0\r")
            assert terminal.finish().status == 0
        finally:
            terminal.kill()

    def _stat() -> tuple[str, str]:
        result = clean_cache.run(
            "stat -c '%i %y' $(find ~/.cache/zet -name zet-agent -perm -u+x | head -1)",
            timeout=STEP_TIMEOUT_SECONDS,
        )
        assert result.returncode == 0, (
            f"stat failed: {result.stderr.decode(errors='replace')}"
        )
        # %i = inode, %y = "YYYY-MM-DD HH:MM:SS.NNNNNNNNN +ZZZZ" (nanoseconds)
        line = result.stdout.decode().strip()
        inode, *rest = line.split(None, 1)
        assert rest, f"unexpected stat output: {result.stdout!r}"
        return inode, rest[0]  # inode, mtime_nanoseconds

    _connect()
    inode_1, mtime_1 = _stat()

    _connect()
    inode_2, mtime_2 = _stat()

    assert inode_1 == inode_2, (
        f"inode changed on second connect: {inode_1!r} -> {inode_2!r}"
    )
    assert mtime_1 == mtime_2, (
        f"mtime changed on second connect (binary rewritten): "
        f"{mtime_1!r} -> {mtime_2!r}"
    )
