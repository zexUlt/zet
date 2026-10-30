"""Delivery mode (M5.5): zet delivers zet-agent to the remote host automatically
over the same ssh -T session used for bootstrap, without a manual scp step.

Every test skips outside the integration container (no sshd / not root).

Scenarios:
  - Fresh host gets agent delivered and session connects.
  - Second connect with same binary copies nothing (inode unchanged).
  - Different binary replaces, old hash dir is cleaned up.
  - ssh killed mid-transfer leaves .part file, no zet-agent.
  - noexec cache dir → clear error, nothing copied.
  - Noisy .bashrc does not confuse the probe exchange.
  - Remote argv passed through ssh is exactly the delivery command.
  - Beside-binary lookup: bin/zet + libexec/zet structure, no ZET_AGENT_DIR.
  - No libexec beside the binary: error names both directories tried.
  - ZET_AGENT_DIR wins over the beside-binary directory.
"""

from __future__ import annotations

import contextlib
import hashlib
import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

from conftest import SHELL_SETUP_ERRORS, SHELL_USERS
from e2e import PROFILE, PROMPT, SANITIZER_OPTIONS
from harness import STEP_TIMEOUT_SECONDS, PtyProcess
from sshhost import CLEAN_BASHRC, NOISY_BASHRC, SSHD, SshHost

# Architecture string uname -m returns inside the container (x86_64, or
# aarch64 on the ARM runner); the agent binary the test installs is named
# for it.
UNAME_ARCH = subprocess.check_output(["uname", "-m"], text=True).strip()
# e_machine of an ELF header, per architecture zet-agent is built for.
ELF_MACHINE = {"x86_64": 0x3E, "aarch64": 0xB7}

# The cache directory the installer creates on the remote host.
ZET_CACHE_SUBDIR = ".cache/zet"


@contextlib.contextmanager
def _throttle_loopback(rate: str = "64mbit", burst: str = "1mbit"):
    """Apply a tc TBF rate limit on lo for the duration of the with-block.

    SSH pre-buffers the entire transfer payload into the kernel's TCP send
    buffer before the remote head -c starts reading; without a rate limit,
    loopback delivers the full binary in milliseconds and the mid-transfer
    kill race is unreliable.  With a 64 Mbit/s shaped rate the transfer
    takes roughly (size / 8 MB/s) seconds; a 16 MiB binary takes ~2s,
    giving the SIGKILL more than a second of window after we detect
    kill_threshold bytes written.

    Fails the test if tc (iproute2) is not installed or the qdisc cannot be
    added: the rate limit is mandatory for a reliable mid-transfer kill test.
    """
    tc = shutil.which("tc")
    if tc is None:
        pytest.fail(
            "tc (iproute2) is not installed — cannot rate-limit loopback; "
            "add iproute2 to tools/integration-packages.txt"
        )
        return  # unreachable; satisfies type checker
    result = subprocess.run(
        [tc, "qdisc", "add", "dev", "lo", "root", "tbf",
         "rate", rate, "burst", burst, "latency", "20ms"],
        capture_output=True, timeout=10,
    )
    if result.returncode != 0:
        pytest.fail(
            f"tc qdisc add failed (rc={result.returncode}): "
            f"{result.stderr.decode(errors='replace').strip()}"
        )
        return  # unreachable
    try:
        yield
    finally:
        subprocess.run(
            [tc, "qdisc", "del", "dev", "lo", "root"],
            check=False, capture_output=True, timeout=10,
        )


def _agent_dir(tmp_path: Path, arch: str, agent_binary: Path) -> Path:
    """Create a ZET_AGENT_DIR with `zet-agent-<arch>` in it."""
    d = tmp_path / "zet-agents"
    d.mkdir(mode=0o755)
    target = d / f"zet-agent-{arch}"
    shutil.copyfile(agent_binary, target)
    target.chmod(0o755)
    return d


def _delivery_argv(client: Path, agent_dir: Path, host: SshHost,
                   agent_env: dict[str, str],
                   *, ssh_env: dict[str, str] | None = None) -> list[str]:
    """zet argv for delivery mode (no --agent flag).

    ssh_env, when given, is merged into the SetEnv passed to OpenSSH so the
    remote installer sees the extra variables (requires AcceptEnv to match).
    """
    all_env = dict(agent_env)
    if ssh_env:
        all_env.update(ssh_env)
    settings = " ".join(f"{name}={value}" for name, value in all_env.items())
    return [
        str(client),
        "--ssh-arg", "-F", "--ssh-arg", str(host.config),
        "--ssh-arg", f"-oSetEnv={settings}",
        host.alias,
    ]


def _env(agent_dir: Path) -> dict[str, str]:
    env = dict(os.environ)
    env.update(SANITIZER_OPTIONS)
    env["ZET_AGENT_DIR"] = str(agent_dir)
    env["ZET_KEEPALIVE_INTERVAL"] = "300"
    env["ZET_PEER_SILENCE_TIMEOUT"] = "2000"
    env["ZET_RECONNECT_MAX"] = "500"
    env["TERM"] = "xterm"
    return env


def _agent_env() -> dict[str, str]:
    return {
        "ZET_KEEPALIVE_INTERVAL": "300",
        "ZET_PEER_SILENCE_TIMEOUT": "2000",
        "ZET_ORPHAN_TIMEOUT": "20",
    }


# ---------------------------------------------------------------------------


@pytest.fixture
def clean_home(ssh_host: SshHost):
    """Remove the zet cache from the test user's home before each test."""
    ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)
    ssh_host.write_home_file(".bash_profile", PROFILE)
    cache_path = ssh_host.home / ZET_CACHE_SUBDIR
    subprocess.run(["rm", "-rf", str(cache_path)], check=False)
    yield ssh_host


def test_fresh_host_delivers_agent_and_session_connects(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """Delivery mode bootstraps a fresh host and opens a working session."""
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
    terminal = PtyProcess(argv, env=_env(agent_dir))
    try:
        terminal.expect(PROMPT)
        # The session is live: run a command.
        terminal.send(b"echo delivered-ok\r")
        terminal.expect(b"delivered-ok")
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()
    # What was installed is an agent for the machine it landed on: the
    # ELF e_machine at offset 18 matches uname -m, on x86_64 and on aarch64.
    installed = list((ssh_host.home / ZET_CACHE_SUBDIR).glob("*/zet-agent"))
    assert len(installed) == 1, installed
    machine = installed[0].read_bytes()[18]
    assert machine == ELF_MACHINE[UNAME_ARCH], (UNAME_ARCH, hex(machine))


def test_second_connect_does_not_retransmit(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """If the binary is already cached (ZET-HAVE), inode and mtime stay the same.

    The client must reply ZET-HAVE on the second connect, not ZET-SEND.
    We verify this indirectly: if the file were rewritten its mtime would
    change.  Both inode and mtime staying equal proves no bytes were sent.
    """
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)

    def connect() -> None:
        """Run one delivery connect and wait for a shell prompt."""
        argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
        terminal = PtyProcess(argv, env=_env(agent_dir))
        try:
            terminal.expect(PROMPT)
            terminal.send(b"exit 0\r")
            assert terminal.finish().status == 0
        finally:
            terminal.kill()

    def stat_agent() -> tuple[str, str]:
        """Return (inode, mtime_seconds) of the cached zet-agent via a plain ssh run."""
        result = ssh_host.run(
            "stat -c '%i %Y' ~/.cache/zet/*/zet-agent",
            timeout=STEP_TIMEOUT_SECONDS,
        )
        assert result.returncode == 0, (
            f"stat failed: {result.stderr.decode(errors='replace')}"
        )
        parts = result.stdout.decode().strip().split()
        assert len(parts) == 2, f"unexpected stat output: {result.stdout!r}"
        return parts[0], parts[1]  # inode, mtime_seconds

    connect()
    inode_1, mtime_1 = stat_agent()

    connect()
    inode_2, mtime_2 = stat_agent()

    assert inode_1 == inode_2, (
        f"inode changed on second connect: {inode_1!r} -> {inode_2!r}"
    )
    assert mtime_1 == mtime_2, (
        f"mtime changed on second connect (binary rewritten): "
        f"{mtime_1!r} -> {mtime_2!r}"
    )


def test_different_binary_replaces_and_old_dir_removed(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """A different binary (different hash) replaces the old one; old hash dir gone."""
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)

    # First connect: delivers original binary.
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
    terminal = PtyProcess(argv, env=_env(agent_dir))
    try:
        terminal.expect(PROMPT)
        terminal.send(b"exit 0\r")
        terminal.kill()
    finally:
        terminal.kill()

    # Create a slightly different binary (append a NOP byte to change hash).
    alt_binary = tmp_path / "zet-agent-alt"
    alt_binary.write_bytes(binaries.agent.read_bytes() + b"\x00")
    alt_binary.chmod(0o755)
    alt_dir = tmp_path / "zet-agents-alt"
    alt_dir.mkdir(mode=0o755)
    alt_target = alt_dir / f"zet-agent-{UNAME_ARCH}"
    shutil.copyfile(alt_binary, alt_target)
    alt_target.chmod(0o755)

    # Second connect with different binary.
    argv2 = _delivery_argv(binaries.client, alt_dir, ssh_host, _agent_env())
    terminal2 = PtyProcess(argv2, env=_env(alt_dir))
    try:
        terminal2.expect(PROMPT)
        terminal2.send(b"exit 0\r")
        terminal2.kill()
    finally:
        terminal2.kill()

    # Count cache dirs via a plain SSH command to avoid pty ANSI colour codes.
    result = ssh_host.run(
        "ls -1 ~/.cache/zet/ 2>/dev/null | wc -l",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    n = result.stdout.decode(errors="replace").strip()
    assert n == "1", f"expected 1 cache dir after replacement, got {n!r}"


def test_mid_transfer_kill_leaves_no_agent(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """Killing the delivery connection mid-transfer leaves no runnable zet-agent.

    When the client process group is SIGKILLed, sshd closes stdin, head -c
    returns a short read, and the installer script removes .part and prints
    ZET-FAILED size.  So after the kill: no zet-agent, no .part file.

    Checks:
    - After the kill, no zet-agent is executable in the cache.
    - No .part file remains (the installer's cleanup ran after stdin closed).
    - A subsequent normal connect with the real agent succeeds.
    - A dir containing a .part whose PID is alive is NOT removed (live-pid).
    - A dir containing a .part whose PID is dead IS removed (dead-pid).
    """
    import signal as _signal

    agent_data = binaries.agent.read_bytes()

    # Pad to ~16 MiB so the transfer takes long enough to kill mid-flight.
    # We wait until the .part file contains at least 1/4 of the binary before
    # killing; at that point 3/4 remains in transit and the rename has not run.
    pad_size = max(0, 16 * 1024 * 1024 - len(agent_data))
    large_data = agent_data + b"\x00" * pad_size
    kill_threshold = len(large_data) // 4
    large_hash = hashlib.blake2b(large_data, digest_size=32).hexdigest()
    large_dir = tmp_path / "large-agents"
    large_dir.mkdir(mode=0o755)
    large_bin = large_dir / f"zet-agent-{UNAME_ARCH}"
    large_bin.write_bytes(large_data)
    large_bin.chmod(0o755)

    # The hash dir the installer will create on the remote side.
    # sshd is on the same host, so we can stat these paths directly.
    hash_dir = ssh_host.home / ZET_CACHE_SUBDIR / large_hash

    large_env = _env(large_dir)
    argv = _delivery_argv(binaries.client, large_dir, ssh_host, _agent_env())

    # Rate-limit loopback so SSH cannot pre-buffer the full binary before
    # head -c starts reading: without this, 16 MiB over loopback completes
    # in < 1 ms and the SIGKILL arrives after the rename.
    with _throttle_loopback():
        # start_new_session=True puts zet and its ssh child in a new process
        # group so we can kill both without affecting the test runner.
        proc = subprocess.Popen(
            argv, env=large_env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=True,
        )

        # Poll the .part.* file size directly on the filesystem (sshd is on
        # the same host, so no extra SSH connection is needed).
        part_appeared = False
        deadline = time.monotonic() + STEP_TIMEOUT_SECONDS
        while time.monotonic() < deadline and proc.poll() is None:
            parts = sorted(hash_dir.glob(".part.*")) if hash_dir.exists() else []
            if parts:
                try:
                    written = parts[0].stat().st_size
                except OSError:
                    written = 0
                if written >= kill_threshold:
                    part_appeared = True
                    break
            time.sleep(0.05)

        # SIGKILL the entire process group (zet + its ssh child).
        try:
            os.killpg(os.getpgid(proc.pid), _signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=STEP_TIMEOUT_SECONDS)

    assert part_appeared, (
        f"delivery completed or failed before {kill_threshold} bytes of "
        f".part appeared; the binary may be too small or the host too fast"
    )

    # Give the remote installer a moment to run its cleanup (rm .part + print
    # ZET-FAILED) after sshd closes stdin.
    deadline2 = time.monotonic() + STEP_TIMEOUT_SECONDS
    while time.monotonic() < deadline2:
        parts_after = sorted(hash_dir.glob(".part.*")) if hash_dir.exists() else []
        agent_path = hash_dir / "zet-agent"
        if not parts_after and not agent_path.exists():
            break
        time.sleep(0.05)

    # No runnable zet-agent must exist.
    assert not agent_path.exists(), (
        f"zet-agent exists after mid-transfer kill: {agent_path}"
    )
    # No .part file: the installer's cleanup removed it after short read.
    assert not parts_after, (
        f".part files remain after mid-transfer kill: {parts_after}"
    )

    # A normal connect with the real agent must succeed.  If any stale .part
    # survived (dead installer PID), kill -0 detects it stale and removes it.
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv2 = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
    terminal = PtyProcess(argv2, env=_env(agent_dir))
    try:
        terminal.expect(PROMPT)
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()

    # --- Dead-pid case ---
    # Plant ~/.cache/zet/<deadhash>/.part.<dead_pid>.  A subsequent install
    # must remove the dir (kill -0 on the dead PID fails → stale).
    # Create the dir as the remote user (via SSH) so the installer's rm -rf
    # has permission to remove it; a root-owned dir cannot be removed by the
    # zettest installer.
    dead_proc = subprocess.Popen(["true"])
    dead_proc.wait()
    dead_pid = dead_proc.pid

    dead_hash = "d" * 64
    dead_dir = ssh_host.home / ZET_CACHE_SUBDIR / dead_hash
    r_dead_setup = ssh_host.run(
        f"mkdir -p ~/.cache/zet/{dead_hash} && "
        f"touch ~/.cache/zet/{dead_hash}/.part.{dead_pid}",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert r_dead_setup.returncode == 0, (
        f"could not create dead-pid .part fixture: "
        f"{r_dead_setup.stderr.decode(errors='replace')}"
    )

    alt_data = agent_data + b"\xde"
    alt_dir = tmp_path / "zet-agents-dead"
    alt_dir.mkdir(mode=0o755)
    alt_bin = alt_dir / f"zet-agent-{UNAME_ARCH}"
    alt_bin.write_bytes(alt_data)
    alt_bin.chmod(0o755)
    argv_dead = _delivery_argv(binaries.client, alt_dir, ssh_host, _agent_env())
    terminal_dead = PtyProcess(argv_dead, env=_env(alt_dir))
    try:
        terminal_dead.expect(PROMPT)
        terminal_dead.send(b"exit 0\r")
        terminal_dead.finish()
    finally:
        terminal_dead.kill()

    assert not dead_dir.exists(), (
        f"dead-pid dir was not removed by a subsequent install: {dead_dir}"
    )

    # --- Live-pid case ---
    # A dir with a LIVE .part must NOT be removed by a different-hash install.
    # Start a long-lived background process *as the remote user* so that
    # kill -0 <pid> from within an SSH session (same user) returns 0.
    live_hash = "a" * 64
    r_sleep = ssh_host.run(
        "sh -c 'sleep 600 </dev/null >/dev/null 2>&1 & echo $!'",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert r_sleep.returncode == 0
    live_pid = r_sleep.stdout.decode().strip()
    live_dir = ssh_host.home / ZET_CACHE_SUBDIR / live_hash
    # Create dir and .part file as the remote user so the installer can
    # manipulate them (root-owned files would be invisible to the cleanup).
    r_live_setup = ssh_host.run(
        f"mkdir -p ~/.cache/zet/{live_hash} && "
        f"touch ~/.cache/zet/{live_hash}/.part.{live_pid}",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert r_live_setup.returncode == 0

    alt2_data = agent_data + b"\xff"
    alt2_dir = tmp_path / "zet-agents-alt"
    alt2_dir.mkdir(mode=0o755)
    alt2_bin = alt2_dir / f"zet-agent-{UNAME_ARCH}"
    alt2_bin.write_bytes(alt2_data)
    alt2_bin.chmod(0o755)
    argv_live = _delivery_argv(binaries.client, alt2_dir, ssh_host, _agent_env())
    terminal_live = PtyProcess(argv_live, env=_env(alt2_dir))
    try:
        terminal_live.expect(PROMPT)
        terminal_live.send(b"exit 0\r")
        terminal_live.kill()
    finally:
        terminal_live.kill()
        ssh_host.run(f"kill {live_pid} 2>/dev/null; true",
                     timeout=STEP_TIMEOUT_SECONDS)

    assert live_dir.exists(), (
        f"live .part dir was removed by a different-hash install: {live_dir}"
    )


def test_noisy_bashrc_does_not_interfere(
    binaries, ssh_host: SshHost, tmp_path
):
    """A .bashrc that emits noise (banners, fake ZET- lines) is handled."""
    ssh_host.write_home_file(".bashrc", NOISY_BASHRC)
    ssh_host.write_home_file(".bash_profile", PROFILE)
    cache_path = ssh_host.home / ZET_CACHE_SUBDIR
    subprocess.run(["rm", "-rf", str(cache_path)], check=False)

    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
    terminal = PtyProcess(argv, env=_env(agent_dir))
    try:
        terminal.expect(PROMPT)
        terminal.send(b"exit 0\r")
        terminal.kill()
    finally:
        terminal.kill()
        ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)


def test_noexec_cache_gives_clear_error(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """When the cache probe reports noexec the client exits with a clear message.

    We inject a fake `chmod` that always exits 1 via the remote user's ~/bin:
    the sshd config sets BASH_ENV=~/.bashrc, so PATH changes reach the installer.
    This substitutes for a real noexec mount (which would require CAP_SYS_ADMIN).

    Nothing must be copied to the cache (no zet-agent, no .part file).
    The error output must contain the word "noexec".
    """
    ssh_host.write_home_file("bin/chmod", "#!/bin/sh\nexit 1\n", mode=0o755)
    ssh_host.write_home_file(".bashrc", 'export PATH="$HOME/bin:$PATH"\n')
    try:
        agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
        argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
        result = subprocess.run(
            argv, env=_env(agent_dir),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=STEP_TIMEOUT_SECONDS,
        )
        assert result.returncode != 0, "expected non-zero exit when cache is noexec"
        combined = result.stdout + result.stderr
        assert b"noexec" in combined.lower(), (
            f"expected 'noexec' in error output, got: {combined!r}"
        )
        # Nothing must have been written to the cache.
        r = ssh_host.run(
            "find ~/.cache/zet/ \\( -name 'zet-agent' -o -name '.part.*' \\) "
            "2>/dev/null | wc -l",
            timeout=STEP_TIMEOUT_SECONDS,
        )
        count = r.stdout.decode().strip()
        assert count == "0", (
            f"files were written to the cache despite noexec error: count={count!r}"
        )
    finally:
        ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)


def test_unsupported_arch_gives_clear_error(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """A fake uname reporting an unknown arch stops delivery with a clear message.

    We use the same .bashrc / ~/bin technique as test_noexec_cache_gives_clear_error.
    The error output must contain both "arch" and "mips64le" (the fake arch value).
    Nothing must be copied to the cache before the error is reported.
    """
    ssh_host.write_home_file("bin/uname", "#!/bin/sh\necho mips64le\n", mode=0o755)
    ssh_host.write_home_file(".bashrc", 'export PATH="$HOME/bin:$PATH"\n')
    try:
        agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
        argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
        result = subprocess.run(
            argv, env=_env(agent_dir),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=STEP_TIMEOUT_SECONDS,
        )
        assert result.returncode != 0, "expected non-zero exit for unsupported arch"
        combined = result.stdout + result.stderr
        # The error must name the unsupported arch so the user knows what to fix.
        assert b"mips64le" in combined, (
            f"expected fake arch 'mips64le' in error output, got: {combined!r}"
        )
        assert b"arch" in combined.lower() or b"unsupported" in combined.lower(), (
            f"expected 'arch' or 'unsupported' in error output, got: {combined!r}"
        )
        # Nothing must have been written to the cache.
        r = ssh_host.run(
            "find ~/.cache/zet/ \\( -name 'zet-agent' -o -name '.part.*' \\) "
            "2>/dev/null | wc -l",
            timeout=STEP_TIMEOUT_SECONDS,
        )
        count = r.stdout.decode().strip()
        assert count == "0", (
            f"files were written to the cache despite unsupported arch: count={count!r}"
        )
    finally:
        ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)


def test_remote_argv_has_no_secrets(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """The remote sh argv is exactly the installer constant; no secrets appear.

    The hash and size are transmitted via stdin, not argv, so they must not
    appear in any running process's command line visible to a local observer.

    Checked:
    - A process with argv exactly ["/bin/sh", "-c", INSTALLER_SCRIPT] was
      observed (the installer ran and was visible in /proc before completing).
    - No /proc cmdline contains the agent hash as a standalone token.
    - Session connected successfully.
    """
    # Load the installer script constant from source to compare against argv.
    hpp = Path("/src/src/client/deliver.hpp")
    if not hpp.exists():
        pytest.skip(f"{hpp} not found — source not mounted at /src")
    installer_script = _extract_installer_script(hpp)

    agent_data = binaries.agent.read_bytes()
    expected_hash = hashlib.blake2b(agent_data, digest_size=32).hexdigest()
    expected_argv2 = installer_script.encode()

    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())

    violations: list[str] = []
    installer_observed = False
    stop_sniffing = threading.Event()

    def sniff_proc() -> None:
        nonlocal installer_observed
        while not stop_sniffing.is_set():
            for pid_dir in Path("/proc").iterdir():
                try:
                    raw = (pid_dir / "cmdline").read_bytes()
                except (PermissionError, FileNotFoundError, OSError):
                    continue
                # Check that installer ran with the expected exact argv.
                parts = raw.split(b"\x00")
                if (len(parts) >= 3
                        and parts[0] in (b"/bin/sh", b"sh")
                        and parts[1] == b"-c"
                        and parts[2] == expected_argv2):
                    installer_observed = True
                # Reject any cmdline containing the hash as a standalone token
                # (not embedded in a path like "/.cache/zet/<hash>/zet-agent").
                text = raw.decode(errors="replace")
                if expected_hash in text and f"/{expected_hash}/" not in text:
                    violations.append(f"pid={pid_dir.name}: {text!r}")
            time.sleep(0.01)

    sniffer = threading.Thread(target=sniff_proc, daemon=True)
    sniffer.start()
    rc = -1
    try:
        terminal = PtyProcess(argv, env=_env(agent_dir))
        try:
            terminal.expect(PROMPT)
            terminal.send(b"exit 0\r")
            rc = terminal.finish().status
        finally:
            terminal.kill()
    finally:
        stop_sniffing.set()
        sniffer.join(timeout=2)

    assert rc == 0, f"session did not succeed (exit {rc})"
    assert installer_observed, (
        "sniffer never observed a process with argv "
        "['/bin/sh', '-c', INSTALLER_SCRIPT] — "
        "either the installer did not run or it completed before the first poll"
    )
    assert not violations, (
        "agent hash found as a standalone argv token during delivery "
        "(should only appear as part of the exec path after install):\n"
        + "\n".join(violations)
    )


@pytest.mark.parametrize("shell", ["ash", "bash", "zsh", "fish", "tcsh"])
def test_login_shell_delivers(
    binaries, shell_ssh_hosts: dict, tmp_path, shell
):
    """Delivery works regardless of the user's login shell."""
    host = shell_ssh_hosts.get(shell)
    if host is None:
        _username, shell_binary = SHELL_USERS[shell]
        if Path(shell_binary).exists():
            # Binary is present but the user was not created: setup failure.
            err = SHELL_SETUP_ERRORS.get(shell, "user absent after setup")
            pytest.fail(
                f"shell {shell!r} binary exists at {shell_binary!r} "
                f"but user setup failed: {err}"
            )
        # In CI the shell packages must be installed; missing binary is a
        # failure, not a skip.
        if os.environ.get("CI"):
            pytest.fail(
                f"shell {shell!r} binary not found at {shell_binary!r} in CI; "
                f"add the package to tools/integration-packages.txt"
            )
        pytest.skip(f"shell {shell!r} not installed at {shell_binary!r}")

    # Clear the cache so the test always does a fresh delivery.
    cache_path = host.home / ZET_CACHE_SUBDIR
    subprocess.run(["rm", "-rf", str(cache_path)], check=False)

    # Write a minimal startup file for each shell so they print a known marker
    # once the session opens.  The delivery itself runs under /bin/sh so these
    # only fire after zet-agent execs the login shell interactively.
    marker = b"ZET_SHELL_READY"
    if shell in ("ash", "bash"):
        host.write_home_file(".profile",
                             f"echo {marker.decode()}\nexport PS1='[zet]$ '\n")
    elif shell == "zsh":
        host.write_home_file(".zprofile",
                             f"echo {marker.decode()}\nexport PS1='[zet]$ '\n")
        host.write_home_file(".zshrc", "# nothing\n")
    elif shell == "fish":
        host.write_home_file(
            ".config/fish/config.fish",
            f"echo {marker.decode()}\nfunction fish_prompt; echo -n '[zet]$ '; end\n",
        )
    elif shell == "tcsh":
        host.write_home_file(".cshrc",
                             f"echo {marker.decode()}\nset prompt='[zet]$ '\n")

    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv = _delivery_argv(binaries.client, agent_dir, host, _agent_env())
    terminal = PtyProcess(argv, env=_env(agent_dir))
    try:
        terminal.expect(marker)
        terminal.send(b"exit 0\r")
        result = terminal.finish()
        assert result.status == 0, (
            f"shell {shell!r}: session exit status {result.status!r}, expected 0"
        )
    finally:
        terminal.kill()


def _extract_installer_script(hpp: Path) -> str:
    """Extract the INSTALLER_SCRIPT string literal from deliver.hpp."""
    text = hpp.read_text()
    # Match: INSTALLER_SCRIPT = <one or more C string literals> ;
    m = re.search(
        r"INSTALLER_SCRIPT\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)\s*;",
        text,
        re.DOTALL,
    )
    if m is None:
        raise ValueError(f"INSTALLER_SCRIPT not found in {hpp}")
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    raw = "".join(parts)
    # Unescape only the C escape sequences that appear in INSTALLER_SCRIPT:
    # \" → " and \\ → \.  Do NOT convert \\n → newline: the constant uses \\n
    # as a printf escape (backslash-n in the shell string), not as a C newline.
    # After replacing \\  →  \, any remaining \n is the literal two-char
    # sequence that printf interprets as a newline — which is correct.
    raw = raw.replace('\\"', '"').replace("\\\\", "\\")
    return raw


def test_installer_script_passes_ash_syntax_check():
    """The installer POSIX sh one-liner is syntactically valid under busybox ash.

    Parses deliver.hpp from the source tree mounted at /src inside the
    integration container, then pipes the script to ash -n (and dash -n if
    dash is available as a small pinned Alpine package).
    """
    if not os.path.exists(SSHD):
        pytest.skip("not inside integration container")

    # /src is where in_alpine.sh mounts the repo root.
    hpp = Path("/src/src/client/deliver.hpp")
    if not hpp.exists():
        pytest.skip(f"{hpp} not found — source not mounted at /src")

    script = _extract_installer_script(hpp)

    result = subprocess.run(
        ["/bin/ash", "-n"],
        input=script.encode(),
        capture_output=True,
    )
    assert result.returncode == 0, (
        f"/bin/ash -n rejected the installer script:\n"
        f"{result.stderr.decode(errors='replace')}"
    )

    dash = shutil.which("dash")
    if dash:
        result2 = subprocess.run(
            [dash, "-n"],
            input=script.encode(),
            capture_output=True,
        )
        assert result2.returncode == 0, (
            f"dash -n rejected the installer script:\n"
            f"{result2.stderr.decode(errors='replace')}"
        )


def test_agent_flag_disables_delivery(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """--agent PATH skips delivery and runs the agent directly at PATH.

    The cache must remain empty after a connect with --agent: no installer ran
    and no zet-agent was written to ~/.cache/zet.
    """
    from e2e import install_agent  # noqa: PLC0415 (local import)

    installed = install_agent(binaries.agent)
    ssh_host.write_home_file(".bash_profile", PROFILE)

    # Build the argv with --agent pointing to the already-installed agent.
    argv = [
        str(binaries.client),
        "--agent", str(installed),
        "--ssh-arg", "-F", "--ssh-arg", str(ssh_host.config),
        ssh_host.alias,
    ]
    env = dict(os.environ)
    env["ZET_KEEPALIVE_INTERVAL"] = "300"
    env["ZET_PEER_SILENCE_TIMEOUT"] = "2000"
    env["ZET_RECONNECT_MAX"] = "500"
    env["TERM"] = "xterm"

    terminal = PtyProcess(argv, env=env)
    try:
        terminal.expect(PROMPT)
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()

    # The cache must be empty: delivery was skipped.
    r = ssh_host.run(
        "find ~/.cache/zet/ -name 'zet-agent' 2>/dev/null | wc -l",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    count = r.stdout.decode().strip()
    assert count == "0", (
        f"zet-agent appeared in cache despite --agent flag: count={count!r}"
    )


def test_agent_dir_missing_arch_gives_clear_error(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """ZET_AGENT_DIR that lacks the remote arch binary gives a clear error.

    The directory exists but contains no zet-agent-<arch> file.  The client
    must exit non-zero and the error must mention ZET_AGENT_DIR or --agent.
    """
    empty_dir = tmp_path / "empty-agents"
    empty_dir.mkdir(mode=0o755)

    env = _env(empty_dir)
    argv = _delivery_argv(binaries.client, empty_dir, ssh_host, _agent_env())
    result = subprocess.run(
        argv, env=env, stdin=subprocess.DEVNULL,
        capture_output=True, timeout=STEP_TIMEOUT_SECONDS,
    )
    assert result.returncode != 0, "expected non-zero exit for missing arch binary"
    combined = result.stdout + result.stderr
    # Must mention ZET_AGENT_DIR or --agent so the user knows how to fix it.
    assert b"ZET_AGENT_DIR" in combined or b"--agent" in combined, (
        f"expected ZET_AGENT_DIR or --agent in error output, got: {combined!r}"
    )


def test_beside_binary_lookup_delivers_without_env(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """Beside-binary lookup: agents in <prefix>/libexec/zet, no ZET_AGENT_DIR.

    Installs the client as <prefix>/bin/zet and agents under
    <prefix>/libexec/zet/zet-agent-<arch>.  ZET_AGENT_DIR is explicitly
    unset.  The client must find the agents by walking up from its own
    location and deliver successfully.

    Proof that the beside-binary path was taken, not the compile-time default:
    the integration container does not have /usr/local/libexec/zet, so a
    successful delivery can only have used the beside-binary directory.
    """
    prefix = tmp_path / "prefix"
    bin_dir = prefix / "bin"
    libexec_dir = prefix / "libexec" / "zet"
    bin_dir.mkdir(parents=True, mode=0o755)
    libexec_dir.mkdir(parents=True, mode=0o755)

    # Install the client as bin/zet.
    client_copy = bin_dir / "zet"
    shutil.copyfile(binaries.client, client_copy)
    client_copy.chmod(0o755)

    # Install the agent binary for the local arch (== the container's arch).
    agent_copy = libexec_dir / f"zet-agent-{UNAME_ARCH}"
    shutil.copyfile(binaries.agent, agent_copy)
    agent_copy.chmod(0o755)

    # Pre-condition: the compile-time default must be absent so the test proves
    # the beside-binary path was used.
    default_agent = Path("/usr/local/libexec/zet") / f"zet-agent-{UNAME_ARCH}"
    if default_agent.exists():
        pytest.skip(
            f"compile-time default {default_agent} exists; "
            "cannot prove beside-binary path was used"
        )

    # Build the env WITHOUT ZET_AGENT_DIR.
    env = dict(os.environ)
    env.update(SANITIZER_OPTIONS)
    env.pop("ZET_AGENT_DIR", None)
    env["ZET_KEEPALIVE_INTERVAL"] = "300"
    env["ZET_PEER_SILENCE_TIMEOUT"] = "2000"
    env["ZET_RECONNECT_MAX"] = "500"
    env["TERM"] = "xterm"

    agent_env = _agent_env()
    settings = " ".join(f"{k}={v}" for k, v in agent_env.items())
    argv = [
        str(client_copy),
        "--ssh-arg", "-F", "--ssh-arg", str(ssh_host.config),
        "--ssh-arg", f"-oSetEnv={settings}",
        ssh_host.alias,
    ]
    terminal = PtyProcess(argv, env=env)
    try:
        terminal.expect(PROMPT)
        terminal.send(b"echo beside-ok\r")
        terminal.expect(b"beside-ok")
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()


def test_zet_agent_dir_wins_over_beside_binary(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """ZET_AGENT_DIR takes priority over the beside-binary directory.

    Installs the client as <prefix>/bin/zet with a *wrong* (empty) agent in
    <prefix>/libexec/zet and a correct agent pointed to by ZET_AGENT_DIR.
    Delivery must succeed using the ZET_AGENT_DIR agent, not the beside-binary
    one.
    """
    prefix = tmp_path / "prefix"
    bin_dir = prefix / "bin"
    libexec_dir = prefix / "libexec" / "zet"
    bin_dir.mkdir(parents=True, mode=0o755)
    libexec_dir.mkdir(parents=True, mode=0o755)

    # Install the client as bin/zet.
    client_copy = bin_dir / "zet"
    shutil.copyfile(binaries.client, client_copy)
    client_copy.chmod(0o755)

    # Plant an intentionally broken zero-byte agent beside the binary so that
    # a fallback to the beside-binary directory would fail.
    broken = libexec_dir / f"zet-agent-{UNAME_ARCH}"
    broken.write_bytes(b"")
    broken.chmod(0o755)

    # Put the real agent in a separate ZET_AGENT_DIR.
    real_agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)

    # ZET_AGENT_DIR must win; env includes it explicitly.
    env = _env(real_agent_dir)

    agent_env = _agent_env()
    argv = _delivery_argv(client_copy, real_agent_dir, ssh_host, agent_env)
    terminal = PtyProcess(argv, env=env)
    try:
        terminal.expect(PROMPT)
        terminal.send(b"echo env-wins-ok\r")
        terminal.expect(b"env-wins-ok")
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()

def test_no_libexec_beside_binary_error_names_both_dirs(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """No libexec/zet beside the binary and no ZET_AGENT_DIR: error names both.

    Installs the client as <prefix>/bin/zet without creating
    <prefix>/libexec/zet.  ZET_AGENT_DIR is unset.  The client must exit
    non-zero and the error must name both the beside-binary directory it tried
    and the compile-time default it also tried (ZET_DEFAULT_AGENT_DIR).
    """
    prefix = tmp_path / "no-libexec-prefix"
    bin_dir = prefix / "bin"
    bin_dir.mkdir(parents=True, mode=0o755)
    # libexec/zet is intentionally absent.

    client_copy = bin_dir / "zet"
    shutil.copyfile(binaries.client, client_copy)
    client_copy.chmod(0o755)

    env = dict(os.environ)
    env.update(SANITIZER_OPTIONS)
    env.pop("ZET_AGENT_DIR", None)
    env["TERM"] = "xterm"

    agent_env = _agent_env()
    settings = " ".join(f"{k}={v}" for k, v in agent_env.items())
    argv = [
        str(client_copy),
        "--ssh-arg", "-F", "--ssh-arg", str(ssh_host.config),
        "--ssh-arg", f"-oSetEnv={settings}",
        ssh_host.alias,
    ]
    result = subprocess.run(
        argv, env=env, stdin=subprocess.DEVNULL,
        capture_output=True, timeout=STEP_TIMEOUT_SECONDS,
    )
    assert result.returncode != 0, "expected non-zero exit when no agents available"
    combined = result.stdout + result.stderr
    # Error must name the beside-binary directory the client computed.
    beside_dir = str(prefix / "libexec" / "zet")
    assert beside_dir.encode() in combined, (
        f"expected beside-binary dir {beside_dir!r} in error, got: {combined!r}"
    )
    # Error must also tell the user how to fix it.
    assert b"ZET_AGENT_DIR" in combined or b"--agent" in combined, (
        f"expected ZET_AGENT_DIR or --agent in error, got: {combined!r}"
    )


def test_libexec_is_a_file_counts_as_absent(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """A file named libexec beside the binary is treated as absent (ENOTDIR).

    Same expectations as the no-libexec case: fall through, exit non-zero
    with the error naming the beside-binary directory.
    """
    prefix = tmp_path / "file-libexec-prefix"
    bin_dir = prefix / "bin"
    bin_dir.mkdir(parents=True, mode=0o755)
    # libexec is a file, so stat() of libexec/zet/... gives ENOTDIR: absent.
    (prefix / "libexec").write_text("not a directory")

    client_copy = bin_dir / "zet"
    shutil.copyfile(binaries.client, client_copy)
    client_copy.chmod(0o755)

    env = dict(os.environ)
    env.update(SANITIZER_OPTIONS)
    env.pop("ZET_AGENT_DIR", None)
    env["TERM"] = "xterm"

    agent_env = _agent_env()
    settings = " ".join(f"{k}={v}" for k, v in agent_env.items())
    argv = [
        str(client_copy),
        "--ssh-arg", "-F", "--ssh-arg", str(ssh_host.config),
        "--ssh-arg", f"-oSetEnv={settings}",
        ssh_host.alias,
    ]
    result = subprocess.run(
        argv, env=env, stdin=subprocess.DEVNULL,
        capture_output=True, timeout=STEP_TIMEOUT_SECONDS,
    )
    assert result.returncode != 0, "expected non-zero exit when no agents available"
    combined = result.stdout + result.stderr
    # Error must name the beside-binary directory the client computed.
    beside_dir = str(prefix / "libexec" / "zet")
    assert beside_dir.encode() in combined, (
        f"expected beside-binary dir {beside_dir!r} in error, got: {combined!r}"
    )
    # Absent, not "exists but unreadable": the fallback was tried too.
    assert b"exists but could not be read" not in combined, combined
    assert b"/usr/local/libexec/zet" in combined, (
        f"expected compile-time default among tried dirs, got: {combined!r}"
    )
    # Error must also tell the user how to fix it.
    assert b"ZET_AGENT_DIR" in combined or b"--agent" in combined, (
        f"expected ZET_AGENT_DIR or --agent in error, got: {combined!r}"
    )
