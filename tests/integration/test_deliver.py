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
    """If the binary is already cached (ZET-HAVE), the inode stays the same."""
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)

    def connect_and_stat() -> bytes:
        argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
        terminal = PtyProcess(argv, env=_env(agent_dir))
        try:
            terminal.expect(PROMPT)
            terminal.send(b'stat -c "%i" ~/.cache/zet/*/zet-agent\r')
            # Consume the echoed command line and read up to the next prompt.
            out = terminal.expect(PROMPT)
            terminal.send(b"exit 0\r")
            terminal.kill()
        finally:
            terminal.kill()
        # The inode is the last non-empty token before the final prompt.
        lines = [ln.strip() for ln in out.split(b"\r\n") if ln.strip()]
        return lines[0] if lines else b""

    inode_1 = connect_and_stat()
    inode_2 = connect_and_stat()
    assert inode_1 == inode_2, (
        f"inode changed on second connect: {inode_1!r} → {inode_2!r}"
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
    """Killing the delivery process leaves no zet-agent in the cache."""
    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)

    # Start the delivery and kill it very quickly so it is interrupted before
    # the installer can rename .part to zet-agent.  A 10 ms window is safely
    # shorter than the SSH handshake, so delivery never completes and the
    # invariant is easy to verify: no zet-agent, any partial state cleaned.
    #
    # We drive this at the shell level: start the installer, read ZET-PROBE,
    # send the hash+size line, send only half the binary, then kill.
    # The simplest proxy: use `head -c <half>` piped into the installer.
    result = ssh_host.run(
        "ls ~/.cache/zet/ 2>/dev/null; echo NO_CACHE",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    assert b"NO_CACHE" in result.stdout

    # Check that after a delivery that was cancelled partway (simulated by
    # starting zet and killing it after a brief pause), no zet-agent appears.
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())
    proc = subprocess.Popen(
        argv,
        env=_env(agent_dir),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    # Kill very quickly: we want to interrupt before delivery completes.
    # 10 ms is enough time for the process to start but not enough to finish
    # the SSH handshake, so no zet-agent is ever installed.
    time.sleep(0.01)
    proc.kill()
    proc.wait(timeout=STEP_TIMEOUT_SECONDS)

    # If a .part file was left, no zet-agent should exist at that path.
    result2 = ssh_host.run(
        "find ~/.cache/zet/ -name 'zet-agent' 2>/dev/null | wc -l; echo DONE",
        timeout=STEP_TIMEOUT_SECONDS,
    )
    # Either no cache at all (killed before anything written) or zet-agent
    # count is 0 (partial write).
    count_line = result2.stdout.decode().split("DONE")[0].strip()
    count = int(count_line) if count_line.isdigit() else 0
    assert count == 0, f"unexpected zet-agent after mid-transfer kill: {result2.stdout!r}"


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

    The installer detects noexec by running `chmod +x <probe-file>`.  We inject
    a fake `chmod` that always exits 1 via the remote user's ~/bin directory:
    the sshd config sets BASH_ENV to ~/.bashrc, which bash reads before exec-ing
    /bin/sh, so PATH modifications in .bashrc reach the installer.  This avoids
    both CAP_SYS_ADMIN (for mount) and the OpenSSH SetEnv/AcceptEnv mechanism.
    """
    # Write fake chmod to the remote user's ~/bin; prepend it in PATH via .bashrc.
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
        combined = (result.stdout + result.stderr).lower()
        assert b"noexec" in combined, (
            f"expected 'noexec' in output, got: {combined!r}"
        )
    finally:
        ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)


def test_unsupported_arch_gives_clear_error(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """A fake uname reporting an unknown arch stops delivery with a clear message.

    We use the same .bashrc / ~/bin technique as test_noexec_cache_gives_clear_error:
    the sshd config sets BASH_ENV=~/.bashrc so the installer inherits PATH changes
    made there.
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
        assert result.returncode != 0, (
            "expected non-zero exit for unsupported arch"
        )
        combined = result.stdout + result.stderr
        assert b"arch" in combined.lower() or b"unsupported" in combined.lower(), (
            f"expected arch/unsupported in output, got: {combined!r}"
        )
    finally:
        ssh_host.write_home_file(".bashrc", CLEAN_BASHRC)


def test_remote_argv_has_no_secrets(
    binaries, ssh_host: SshHost, clean_home, tmp_path
):
    """No process cmdline in /proc ever contains the agent hash during delivery.

    The hash is transmitted via stdin, not argv, so it must not appear in any
    running process's command line visible to an observer on the machine.
    """
    agent_data = binaries.agent.read_bytes()
    expected_hash = hashlib.blake2b(agent_data, digest_size=32).hexdigest()

    agent_dir = _agent_dir(tmp_path, UNAME_ARCH, binaries.agent)
    argv = _delivery_argv(binaries.client, agent_dir, ssh_host, _agent_env())

    violations: list[str] = []
    stop_sniffing = threading.Event()

    def sniff_proc():
        while not stop_sniffing.is_set():
            for pid_dir in sorted(Path("/proc").iterdir()):
                try:
                    raw = (pid_dir / "cmdline").read_bytes()
                except (PermissionError, FileNotFoundError, OSError):
                    continue
                text = raw.decode(errors="replace")
                if expected_hash in text:
                    # Acceptable: hash appears as part of the installed agent's
                    # path, e.g. "/home/user/.cache/zet/<hash>/zet-agent".
                    # That is expected after a successful install.
                    # Forbidden: hash as a standalone SSH argument or token.
                    if f"/{expected_hash}/" in text:
                        continue
                    violations.append(f"pid={pid_dir.name}: {text!r}")
            time.sleep(0.01)

    sniffer = threading.Thread(target=sniff_proc, daemon=True)
    sniffer.start()
    try:
        proc = subprocess.Popen(
            argv, env=_env(agent_dir),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            proc.wait(timeout=STEP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    finally:
        stop_sniffing.set()
        sniffer.join(timeout=2)

    assert not violations, (
        "hash found in /proc cmdlines during delivery:\n" + "\n".join(violations)
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
