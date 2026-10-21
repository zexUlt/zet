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
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

from e2e import PROFILE, PROMPT, SANITIZER_OPTIONS
from harness import STEP_TIMEOUT_SECONDS, PtyProcess
from sshhost import CLEAN_BASHRC, NOISY_BASHRC, SshHost

# Architecture string uname -m returns inside the container (always x86_64
# on CI); the agent binary the test installs is named for it.
UNAME_ARCH = subprocess.check_output(["uname", "-m"], text=True).strip()

# The cache directory the installer creates on the remote host.
ZET_CACHE_SUBDIR = ".cache/zet"


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


def test_mid_transfer_kill_leaves_only_part_file(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """Killing the delivery connection mid-transfer leaves no runnable zet-agent.

    We use a padded (2 MiB) binary so the transfer takes long enough that we
    can reliably kill the process group while head -c is still running on the
    remote side.  The kill leaves a .part.* file and no zet-agent.

    Checks:
    - After the kill, no zet-agent is executable in the cache.
    - A .part.* file exists in the hash dir (partial transfer recorded).
    - A subsequent normal connect with the real agent succeeds (stale .part
      from the dead installer PID is cleaned up by the stale-check logic).
    - A dir containing a .part file whose PID is still alive is NOT removed
      by a different-hash install.
    """
    import signal as _signal

    agent_data = binaries.agent.read_bytes()

    # Pad to ~2 MiB so the transfer takes > 200 ms on loopback.
    pad_size = max(0, 2 * 1024 * 1024 - len(agent_data))
    large_data = agent_data + b"\x00" * pad_size
    large_hash = hashlib.blake2b(large_data, digest_size=32).hexdigest()
    large_dir = tmp_path / "large-agents"
    large_dir.mkdir(mode=0o755)
    large_bin = large_dir / f"zet-agent-{UNAME_ARCH}"
    large_bin.write_bytes(large_data)
    large_bin.chmod(0o755)

    large_env = _env(large_dir)
    argv = _delivery_argv(binaries.client, large_dir, ssh_host, _agent_env())
    # start_new_session=True puts zet and its ssh child in a new process group
    # so we can kill both without affecting the test runner.
    proc = subprocess.Popen(
        argv, env=large_env, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )

    # Poll until the .part.* file appears in the cache (head -c has started
    # writing), or the process exits first (transfer completed / failed early).
    part_appeared = False
    deadline = time.monotonic() + STEP_TIMEOUT_SECONDS
    while time.monotonic() < deadline and proc.poll() is None:
        r = ssh_host.run(
            f"ls ~/.cache/zet/{large_hash}/.part.* 2>/dev/null | wc -l",
            timeout=5,
        )
        if r.returncode == 0 and r.stdout.decode().strip() != "0":
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
        "delivery completed or failed before a .part file appeared; "
        "the binary may be too small or the host too fast"
    )

    # No runnable zet-agent must exist.
    r2 = ssh_host.run(
        f"test -x ~/.cache/zet/{large_hash}/zet-agent && echo FOUND || echo ABSENT",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert b"ABSENT" in r2.stdout, (
        f"zet-agent is executable after mid-transfer kill: {r2.stdout!r}"
    )

    # A normal connect with the real agent must succeed.  The stale .part
    # (PID of the now-dead sh process) is treated as stale by kill -0 and
    # cleaned up during the next install's cleanup pass.
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv2 = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
    terminal = PtyProcess(argv2, env=_env(agent_dir))
    try:
        terminal.expect(PROMPT)
        terminal.send(b"exit 0\r")
        assert terminal.finish().status == 0
    finally:
        terminal.kill()

    # A dir with a LIVE .part must not be removed by a different-hash install.
    # Use the test process's own PID: it is alive on the (same) machine, so
    # kill -0 on the remote side succeeds and the dir is protected.
    fake_hash = "a" * 64
    r3 = ssh_host.run(
        f"mkdir -p ~/.cache/zet/{fake_hash} && "
        f"touch ~/.cache/zet/{fake_hash}/.part.{os.getpid()}",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert r3.returncode == 0

    alt_data = agent_data + b"\xff"
    alt_dir = tmp_path / "zet-agents-alt"
    alt_dir.mkdir(mode=0o755)
    alt_bin = alt_dir / f"zet-agent-{UNAME_ARCH}"
    alt_bin.write_bytes(alt_data)
    alt_bin.chmod(0o755)
    argv3 = _delivery_argv(binaries.client, alt_dir, ssh_host, _agent_env())
    terminal2 = PtyProcess(argv3, env=_env(alt_dir))
    try:
        terminal2.expect(PROMPT)
        terminal2.send(b"exit 0\r")
        terminal2.kill()
    finally:
        terminal2.kill()

    r4 = ssh_host.run(
        f"test -d ~/.cache/zet/{fake_hash} && echo ALIVE || echo GONE",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert b"ALIVE" in r4.stdout, (
        f"live .part dir was removed by a different-hash install: {r4.stdout!r}"
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

    Expected in argv (not secret):
    - The installer script text itself (a constant, no variable data).
    - The hash as part of the installed agent path after a successful install:
      e.g. "/home/user/.cache/zet/<hash>/zet-agent" — expected and acceptable.
    - "head -c <size>" as part of the installer script literal — acceptable
      because the size is fixed by the script text, not the current binary.

    Forbidden in any /proc cmdline during delivery:
    - The 64-char hex hash as a standalone argument or token (not in a path).
    - Any key, nonce, or Offer material (bootstrap secrets, not yet generated
      during delivery).

    The test also verifies that the session connected successfully.
    """
    agent_data = binaries.agent.read_bytes()
    expected_hash = hashlib.blake2b(agent_data, digest_size=32).hexdigest()

    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())

    violations: list[str] = []
    stop_sniffing = threading.Event()

    def sniff_proc() -> None:
        while not stop_sniffing.is_set():
            for pid_dir in sorted(Path("/proc").iterdir()):
                try:
                    raw = (pid_dir / "cmdline").read_bytes()
                except (PermissionError, FileNotFoundError, OSError):
                    continue
                text = raw.decode(errors="replace")
                if expected_hash not in text:
                    continue
                # Acceptable: hash in the installed agent's exec path after a
                # successful install, e.g. "…/.cache/zet/<hash>/zet-agent".
                if f"/{expected_hash}/" in text:
                    continue
                violations.append(f"pid={pid_dir.name}: {text!r}")
            time.sleep(0.01)

    sniffer = threading.Thread(target=sniff_proc, daemon=True)
    sniffer.start()
    try:
        terminal = PtyProcess(argv, env=_env(agent_dir))
        try:
            # Session must connect successfully: if the argv contained a
            # secret, the installer or bootstrap would likely fail too.
            terminal.expect(PROMPT)
            terminal.send(b"exit 0\r")
            rc = terminal.finish().status
        finally:
            terminal.kill()
    finally:
        stop_sniffing.set()
        sniffer.join(timeout=2)

    assert rc == 0, f"session did not succeed (exit {rc})"
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
        pytest.skip(f"shell {shell!r} not installed or user not created")

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
        terminal.kill()
    finally:
        terminal.kill()


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

    # Build the ssh argv with --agent pointing to the already-installed agent.
    ssh_args = ["-F", str(ssh_host.config)]
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
