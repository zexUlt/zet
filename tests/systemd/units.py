"""The installed units and what the systemd suite asks of them. A module of
its own rather than in conftest.py: a test that imports conftest gets a second
copy of it."""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

from harness import STEP_TIMEOUT_SECONDS, wait_until

# Where the workflow installs the three binaries, as the units expect.
BIN_DIR = Path(os.environ.get("ZET_SYSTEMD_BIN_DIR", "/usr/local/bin"))

SOCKET_UNIT = "zet-muxd.socket"
SERVICE_UNIT = "zet-muxd.service"

# What units/zet-muxd.socket listens on and the service logs to.
MUX_PORT = 2022
MUX_SOCKET = Path("/run/zet/mux.sock")
MUX_LOG = Path("/var/log/zet/muxd.log")

SECOND_USER = "zettest2"


def systemctl(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["systemctl", *args], check=check, capture_output=True,
                          text=True, timeout=STEP_TIMEOUT_SECONDS)


@dataclass
class Units:
    """The installed units and what the tests ask them."""

    def state(self, unit: str) -> str:
        return systemctl("is-active", unit, check=False).stdout.strip()

    def main_pid(self) -> int:
        out = systemctl("show", "--property=MainPID", "--value", SERVICE_UNIT).stdout
        return int(out.strip() or 0)

    def wait_running(self) -> int:
        """The service's pid once systemd reports it running. systemd has a
        D-Bus signal for this, but nothing in the stdlib listens to D-Bus:
        hence the poll."""
        wait_until(lambda: self.state(SERVICE_UNIT) == "active" and self.main_pid() > 0,
                   f"{SERVICE_UNIT} to run")
        return self.main_pid()

    def log(self) -> str:
        try:
            return MUX_LOG.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def registrations(self) -> list[tuple[str, int]]:
        found = []
        for line in self.log().splitlines():
            if "registered sid=" not in line:
                continue
            fields = dict(part.split("=", 1) for part in line.split() if "=" in part)
            found.append((fields["sid"], int(fields["uid"])))
        return found

    def wait_registered(self, uid: int, after: int = 0) -> str:
        """The first registration by `uid` past the first `after`. The log is
        a file the mux appends to: no event for that, hence the poll."""
        def found() -> list[str]:
            return [sid for sid, owner in self.registrations()[after:] if owner == uid]
        wait_until(lambda: bool(found()), f"a registration by uid {uid}")
        return found()[0]

    def journal(self) -> str:
        return subprocess.run(["journalctl", "--no-pager", "-n", "80", "-u", SOCKET_UNIT,
                               "-u", SERVICE_UNIT], capture_output=True, text=True,
                              timeout=STEP_TIMEOUT_SECONDS).stdout


