"""What the M4 scenarios stand on, checked before there is a zet to run.

Each test here pins one property the later ones assume: that ssh to the
test user works and is not root, that the ssh channel carries binary data
whole past a noisy .bashrc (the channel bootstrap rides, §4), that a pty
reaches the far side, and that iptables can make a connection half-open.
"""

from __future__ import annotations

import os
import random
import select
import socket
import subprocess

import pytest

from harness import STEP_TIMEOUT_SECONDS, PtyProcess, Timeout, wait_process
from conftest import NOISE_BANNER

# More than one pipe buffer each way, so that the copy below has to keep
# both directions moving instead of writing everything first.
CHANNEL_BYTES = 1024 * 1024
CHANNEL_SEED = 2024

# How long a dropped segment is given to arrive anyway before the test
# believes it was dropped.
DROP_WINDOW_SECONDS = 1.0


def _pump(process: subprocess.Popen, data: bytes) -> bytes:
    """Feeds `data` to the process and collects its stdout to the end, both
    at once and with a deadline, the way a terminal would."""
    out = bytearray()
    view = memoryview(data)
    stdin, stdout = process.stdin.fileno(), process.stdout.fileno()
    os.set_blocking(stdin, False)
    open_out = True
    while open_out:
        writers = [stdin] if view else []
        readable, writable, _ = select.select([stdout], writers, [], STEP_TIMEOUT_SECONDS)
        if not readable and not writable:
            raise Timeout(f"ssh stalled: {len(view)} bytes unsent, {len(out)} received")
        if writable:
            view = view[os.write(stdin, view[:65536]):]
            if not view:
                process.stdin.close()
        if readable:
            chunk = os.read(stdout, 65536)
            if chunk:
                out += chunk
            else:
                open_out = False
    wait_process(process)
    return bytes(out)


def test_ssh_logs_in_as_an_ordinary_user(clean_bashrc):
    result = clean_bashrc.run("id -u; id -un")
    assert result.returncode == 0, result.stderr
    uid, name = result.stdout.decode().split()
    assert name == clean_bashrc.user
    assert uid != "0"


def test_noisy_bashrc_is_read_on_a_command_login(noisy_bashrc):
    """The noise has to reach the channel, or the bootstrap test that relies
    on it checks nothing."""
    result = noisy_bashrc.run("true")
    assert NOISE_BANNER in result.stdout
    assert b"-----BEGIN ZET BOOTSTRAP-----" in result.stdout
    assert b"noise on stderr" in result.stderr


def test_ssh_channel_carries_binary_whole_past_the_noise(noisy_bashrc):
    data = random.Random(CHANNEL_SEED).randbytes(CHANNEL_BYTES)
    process = subprocess.Popen(noisy_bashrc.argv("-T") + ["cat"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
    out = _pump(process, data)
    assert process.returncode == 0
    # The rc file speaks first; after it, the channel is a clean byte pipe.
    assert out.startswith(b"Welcome to the noisy host")
    assert out.endswith(data)


def test_a_pty_reaches_the_far_side_with_its_size(clean_bashrc):
    session = PtyProcess(clean_bashrc.argv("-tt") + ["stty size"], rows=33, cols=101)
    try:
        finished = session.finish()
    finally:
        session.kill()
    assert finished.status == 0
    assert b"33 101" in finished.output


def test_iptables_drop_makes_an_established_connection_half_open(iptables):
    with socket.create_server(("127.0.0.1", 0)) as listener:
        port = listener.getsockname()[1]
        client = socket.create_connection(("127.0.0.1", port), timeout=STEP_TIMEOUT_SECONDS)
        server, _ = listener.accept()
        rule = ["INPUT", "-i", "lo", "-p", "tcp", "--dport", str(port), "-j", "DROP"]
        subprocess.run([iptables, "-I", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        try:
            client.sendall(b"into the void")
            # Neither delivered nor refused: the segment is gone, and the
            # sender learns nothing. That is the half-open a reset would hide.
            readable, _, _ = select.select([server], [], [], DROP_WINDOW_SECONDS)
            assert not readable
        finally:
            subprocess.run([iptables, "-D", *rule], check=True, timeout=STEP_TIMEOUT_SECONDS)
        # TCP retransmits once the path is back, and the bytes arrive whole.
        readable, _, _ = select.select([server], [], [], STEP_TIMEOUT_SECONDS)
        assert readable
        assert server.recv(64) == b"into the void"
        client.close()
        server.close()


def test_the_binaries_are_there(binaries):
    """Skips until zet and zet-agent are built; then every scenario can run."""
    for path in (binaries.client, binaries.agent):
        assert os.access(path, os.X_OK), path
