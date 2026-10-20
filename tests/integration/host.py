"""Host mode: the test host of the integration suite, for a client outside
the container (design.md §16, the macOS e2e job).

Runs in the Alpine container as its main process: creates the test user and
fresh keys, installs zet-agent, starts sshd on all addresses, writes what
the client needs into a directory shared with it, prints READY_LINE and
stays until it is told to stop. Paths and the address in the client's
ssh_config are given as the client sees them, which is not as the container
does.

    python3 tests/integration/host.py --agent BUILT --out DIR \\
        --client-dir DIR_AS_THE_CLIENT_SEES_IT --client-port PORT
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
from pathlib import Path

from e2e import AGENT_DIR, install_agent
from sshhost import (HOST_ALIAS, TEST_USER, ensure_user, make_keys,
                     start_sshd, stop_sshd, write_client_config)

# What tools/macos_e2e_host.sh waits for in the container's log.
READY_LINE = "zet-e2e-host ready"

# sshd's port inside the container. The client sees the published one.
SSHD_PORT = 2222

# Where the client's description of the host goes, next to its ssh_config.
DESCRIPTION = "host.json"


def _hand_to_client(path: Path, mode: int) -> None:
    # The directory is the client's, mounted into the container: files root
    # makes there must be the client user's, and the key only theirs, or ssh
    # on that side refuses it.
    uid = int(os.environ.get("ZET_UID", "-1"))
    gid = int(os.environ.get("ZET_GID", "-1"))
    if uid >= 0:
        os.chown(path, uid, gid)
    path.chmod(mode)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", required=True, type=Path,
                        help="zet-agent as built, inside the container")
    parser.add_argument("--out", required=True, type=Path,
                        help="the shared directory, as the container sees it")
    parser.add_argument("--client-dir", required=True, type=Path,
                        help="the same directory, as the client sees it")
    parser.add_argument("--client-host", default="127.0.0.1",
                        help="the address the client reaches sshd at")
    parser.add_argument("--client-port", required=True, type=int,
                        help="the port sshd is published on for the client")
    parser.add_argument("--agent-ports", required=True,
                        help="the agent port range published, FIRST-LAST")
    parser.add_argument("--container", required=True,
                        help="this container's name, for docker exec")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    user = ensure_user()
    home = Path(user.pw_dir)
    keys = make_keys(args.out, user)
    _hand_to_client(keys.client, 0o600)
    _hand_to_client(args.out / "client_key.pub", 0o644)
    agent = install_agent(args.agent)
    sshd, _ = start_sshd(args.out, keys, home, "0.0.0.0", SSHD_PORT)

    config = write_client_config(
        args.out / "ssh_config", hostname=args.client_host,
        port=args.client_port, identity=args.client_dir / "client_key",
        known_hosts=args.out / "known_hosts",
        host_public_key=(args.out / "host_key.pub").read_text(),
        known_hosts_as=args.client_dir / "known_hosts")
    _hand_to_client(config, 0o644)
    _hand_to_client(args.out / "known_hosts", 0o644)

    description = args.out / DESCRIPTION
    description.write_text(json.dumps({
        "alias": HOST_ALIAS,
        "user": TEST_USER,
        "home": str(home),
        "ssh_config": str(args.client_dir / "ssh_config"),
        "agent": str(agent),
        "agent_dir": str(AGENT_DIR),
        "agent_ports": args.agent_ports,
        "container": args.container,
    }, indent=2) + "\n")
    _hand_to_client(description, 0o644)

    def stop(_signal: int, _frame) -> None:
        stop_sshd(sshd)
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    print(READY_LINE, flush=True)
    # Waits on sshd, not on a timer: the container lives as long as it does.
    return sshd.wait()


if __name__ == "__main__":
    sys.exit(main())
