"""What the macOS scenarios stand on, checked before there is a macOS zet:
that this side reaches the host over ssh, that the channel carries binary
whole past the noisy .bashrc, that a port the agent would pick is reachable
from here as it is, and that iptables in the host can cut it.
"""

from __future__ import annotations

import os
import random
import select
import shlex
import socket
import subprocess

from harness import NOISE_BANNER, STEP_TIMEOUT_SECONDS, Timeout

CHANNEL_BYTES = 1024 * 1024
CHANNEL_SEED = 2026

# How long a dropped segment is given to arrive anyway before the test
# believes it was dropped.
DROP_WINDOW_SECONDS = 1.0

# A listener as the agent makes one: dual-stack, on an ephemeral port, which
# the host's narrowed range puts among the published ones. It prints the
# port, answers one connection with what it reads back, and ends.
ECHO_LISTENER = r"""
import socket, sys
s = socket.socket(socket.AF_INET6)
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
s.bind(("::", 0))
s.listen()
print(s.getsockname()[1], flush=True)
c, _ = s.accept()
while True:
    data = c.recv(65536)
    if not data:
        break
    c.sendall(data)
"""


def _pump(process: subprocess.Popen, data: bytes) -> bytes:
    """Feeds `data` to the process and collects its stdout to the end, both
    at once and with a deadline."""
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
    process.wait(timeout=STEP_TIMEOUT_SECONDS)
    return bytes(out)


class _Listener:
    """ECHO_LISTENER on the host, started over ssh."""

    def __init__(self, host):
        self.process = subprocess.Popen(host.argv("-T") + ["python3 -c " + shlex.quote(ECHO_LISTENER)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready, _, _ = select.select([self.process.stdout], [], [], STEP_TIMEOUT_SECONDS)
        if not ready:
            self.process.kill()
            raise Timeout("the listener did not report its port")
        line = self.process.stdout.readline()
        if not line:
            # Readable at end of file too: the listener died before its port.
            self.process.kill()
            raise Timeout("the listener exited without reporting its port")
        self.port = int(line)

    def close(self):
        self.process.kill()
        self.process.wait(timeout=STEP_TIMEOUT_SECONDS)


def test_this_side_reaches_the_host_as_an_ordinary_user(clean_bashrc):
    result = clean_bashrc.run("id -u; id -un")
    assert result.returncode == 0, result.stderr
    uid, name = result.stdout.decode().split()
    assert name == clean_bashrc.user
    assert uid != "0"


def test_the_agent_is_installed_for_the_test_user(clean_bashrc):
    result = clean_bashrc.run(f"test -x '{clean_bashrc.agent}' && echo found")
    assert result.stdout.strip() == b"found", result.stderr


def test_the_channel_carries_binary_whole_past_the_noise(noisy_bashrc):
    data = random.Random(CHANNEL_SEED).randbytes(CHANNEL_BYTES)
    process = subprocess.Popen(noisy_bashrc.argv("-T") + ["cat"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
    out = _pump(process, data)
    assert process.returncode == 0
    assert out.startswith(NOISE_BANNER)
    assert out.endswith(data)


def test_a_port_the_agent_would_pick_is_reachable_as_it_is(clean_bashrc):
    """The agent announces the port it bound, and the client dials that port
    at the address ssh resolves to. The host publishes its whole ephemeral
    range under the same numbers, so that works without the agent knowing."""
    listener = _Listener(clean_bashrc)
    try:
        assert listener.port in clean_bashrc.agent_ports
        with socket.create_connection(("127.0.0.1", listener.port),
                                      timeout=STEP_TIMEOUT_SECONDS) as client:
            client.sendall(b"through the published range")
            assert client.recv(64) == b"through the published range"
    finally:
        listener.close()


def test_iptables_in_the_host_cuts_a_connection_from_this_side(iptables):
    listener = _Listener(iptables)
    try:
        with socket.create_connection(("127.0.0.1", listener.port),
                                      timeout=STEP_TIMEOUT_SECONDS) as client:
            client.sendall(b"before")
            assert client.recv(64) == b"before"
            rule = ["INPUT", "-i", "eth0", "-p", "tcp", "--dport",
                    str(listener.port), "-j", "DROP"]
            assert iptables.exec(["iptables", "-I", *rule]).returncode == 0
            try:
                client.sendall(b"into the void")
                readable, _, _ = select.select([client], [], [], DROP_WINDOW_SECONDS)
                assert not readable
            finally:
                assert iptables.exec(["iptables", "-D", *rule]).returncode == 0
            # Retransmitted once the path is back, and echoed whole.
            readable, _, _ = select.select([client], [], [], STEP_TIMEOUT_SECONDS)
            assert readable
            assert client.recv(64) == b"into the void"
    finally:
        listener.close()


def test_the_macos_client_is_there(macos_zet):
    """Skips until the macOS port builds zet; then the scenarios can run."""
    assert os.access(macos_zet, os.X_OK)
