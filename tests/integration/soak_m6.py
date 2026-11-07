"""The M6 soak (design.md §16): three sessions with different output rates,
a forced path B every cycle, and the agents' RSS held against the bound of the
criterion.

Not collected by the normal run: the name does not start with `test_`, and the
workflow .github/workflows/m6-soak.yml names the file. The settings come from
the environment (tools/integration.sh forwards every ZET_SOAK_*):

    ZET_SOAK_SECONDS         whole run                              default 14400
    ZET_SOAK_CYCLE_SECONDS   between two forced path B cuts        default 1800
    ZET_SOAK_SAMPLE_SECONDS  between two RSS samples                default 30
    ZET_SOAK_FACTOR          the N of the criterion                 default 2
    ZET_SOAK_OUT             where the series and summary go        default build/soak-m6

A cycle cuts all three links, has each shell print more than the agent's
detached buffer holds (ZET_DISCONNECT_BUFFER_MAX is left at its default, so the
ring really fills to 16 MiB), lifts the cuts and waits for every client to
apply a snapshot. The agents run with their default scrollback and buffers, so
the bound is built from the constants of the source, not from test settings.

Output, all under ZET_SOAK_OUT: rss.csv (a sample per session per interval),
snapshots.csv (one row per snapshot, from the agents' own log lines), and
summary.json / summary.md.
"""

from __future__ import annotations

import csv
import json
import os
import re
import time
import uuid
from contextlib import ExitStack
from dataclasses import dataclass, field
from pathlib import Path

import pytest

from e2e import PROMPT, REGAINED_NOTICE, Terminal, Zet, connection_to_agent, parent_of, socket_inodes, sockets_of, \
    TCP_ESTABLISHED
from m6_support import COLS, ROWS, SNAPSHOT_NOTICE, link_cut, shell_pid, typed

REPO = Path(__file__).resolve().parents[2]

SOAK_SECONDS = int(os.environ.get("ZET_SOAK_SECONDS", 4 * 3600))
CYCLE_SECONDS = int(os.environ.get("ZET_SOAK_CYCLE_SECONDS", 30 * 60))
SAMPLE_SECONDS = int(os.environ.get("ZET_SOAK_SAMPLE_SECONDS", 30))
FACTOR = float(os.environ.get("ZET_SOAK_FACTOR", 2))
OUT = Path(os.environ.get("ZET_SOAK_OUT", "build/soak-m6")).resolve()

# `seq 1 3000000` is 22.8 MB: more than DISCONNECT_BUFFER_MAX (16 MiB).
BURST_LINES = 3_000_000
KIB = 1024

# The producers run in the background of each shell, so that its foreground
# stays free for the burst. Bytes per second, roughly: 30, 7 000 and 600 000.
PRODUCERS = {
    "slow": "while :; do date; sleep 1; done",
    "medium": "while :; do seq 1 300; sleep 0.1; done",
    "fast": "while :; do seq 1 5000; sleep 0.05; done",
}

# A cycle that takes longer than this to recover is a hang, not a slow run.
CYCLE_DEADLINE_SECONDS = 300
# After the last session has recovered: let the agent settle before the sample
# that stands for the cycle.
SETTLE_SECONDS = 5

# A trend is judged on the cycle-end samples after this many warm-up cycles,
# and only from MIN_TREND_POINTS of them. A leak of the idInfoMap class grows
# with every reconnect; allocator noise does not, and stays well under the
# slack.
WARMUP_CYCLES = 2
MIN_TREND_POINTS = 5
TREND_SLACK_KIB = 4 * KIB

SNAPSHOT_LINE = re.compile(r"grid snapshot supplied bytes=(\d+) scrollback_bytes=(\d+) flags=(\d+)")


def constant(path: str, name: str) -> int:
    """The value of `inline constexpr T NAME = <integer expression>;` in a
    header of the source, so that the bound follows the code."""
    text = (REPO / path).read_text()
    match = re.search(rf"constexpr\s+[\w:]+\s+{name}\s*=\s*([^;]+);", text)
    assert match, f"{name} not found in {path}"
    expression = re.sub(r"(?<=\d)[UL]+\b", "", match.group(1))
    assert re.fullmatch(r"[0-9 *<()+]+", expression), expression
    return int(eval(expression, {"__builtins__": {}}))  # noqa: S307 — checked above


def cell_size() -> int:
    text = (REPO / "src/grid/include/zet/grid/cell.hpp").read_text()
    match = re.search(r"static_assert\(sizeof\(Cell\) == (\d+)", text)
    assert match, "sizeof(Cell) assertion not found"
    return int(match.group(1))


def proc_kib(pid: int, field_name: str) -> int:
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(field_name + ":"):
            return int(line.split()[1])
    raise AssertionError(f"no {field_name} for pid {pid}")


@dataclass
class Session:
    name: str
    terminal: Terminal
    agent: int
    log: Path
    base_kib: int = 0
    regained: int = 0
    restored: int = 0
    hwm_kib: int = 0
    cycle_end_kib: list[int] = field(default_factory=list)

    def read(self) -> None:
        """Takes what the terminal has, counts zet's notices in it, and keeps
        the buffer from growing without bound: nothing here reads it."""
        before = len(self.terminal.buffer)
        got = self.terminal.read_more(0)
        if got == 0:
            raise AssertionError(f"session {self.name} ended: {self.terminal.tail()!r}")
        for notice, attribute in ((REGAINED_NOTICE, "regained"), (SNAPSHOT_NOTICE, "restored")):
            start = max(0, before - len(notice) + 1)
            setattr(self, attribute,
                    getattr(self, attribute) + self.terminal.buffer.count(notice, start))
        if len(self.terminal.buffer) > 1024 * KIB:
            del self.terminal.buffer[:-4 * KIB]
            self.terminal.consumed = 0


def agent_detached(session: Session, zet: Zet) -> bool:
    return not any(s.state == TCP_ESTABLISHED for s in sockets_of(session.agent, zet.host.user))


class Soak:
    def __init__(self, zet: Zet, iptables: str, sessions: list[Session], bound_kib: int) -> None:
        self.zet = zet
        self.iptables = iptables
        self.sessions = sessions
        self.bound_kib = bound_kib
        self.started = time.monotonic()
        self.next_sample = self.started
        self.rows: list[dict] = []
        OUT.mkdir(parents=True, exist_ok=True)
        self.csv_file = open(OUT / "rss.csv", "w", newline="")
        self.csv = csv.writer(self.csv_file)
        self.csv.writerow(["t_seconds", "session", "rss_kib", "hwm_kib", "bound_kib"])

    def now(self) -> float:
        return time.monotonic() - self.started

    def pump(self, seconds: float) -> None:
        """Reads every session for `seconds` and samples RSS when due."""
        import select
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            masters = {s.terminal.process.master: s for s in self.sessions}
            ready, _, _ = select.select(list(masters), [], [], max(0.0, min(left, 0.2)))
            for fd in ready:
                masters[fd].read()
            self.sample_if_due()
            if left <= 0:
                return

    def pump_until(self, condition, what: str, deadline: float = CYCLE_DEADLINE_SECONDS) -> None:
        end = time.monotonic() + deadline
        while not condition():
            if time.monotonic() >= end:
                raise AssertionError(f"{what} not within {deadline}s")
            self.pump(0.2)

    def sample_if_due(self) -> None:
        if time.monotonic() < self.next_sample:
            return
        self.next_sample += SAMPLE_SECONDS
        self.sample()

    def sample(self) -> dict[str, int]:
        taken = {}
        for s in self.sessions:
            rss, hwm = proc_kib(s.agent, "VmRSS"), proc_kib(s.agent, "VmHWM")
            s.hwm_kib = hwm
            taken[s.name] = rss
            self.csv.writerow([f"{self.now():.0f}", s.name, rss, hwm, self.bound_kib])
            assert hwm <= self.bound_kib, (
                f"session {s.name}: peak RSS {hwm} KiB exceeds the bound {self.bound_kib} KiB "
                f"(base {s.base_kib} KiB) at t={self.now():.0f}s")
        self.csv_file.flush()
        return taken

    def cycle(self, number: int) -> None:
        """One forced path B on every session at once."""
        tag = uuid.uuid4().hex
        paths = {s.name: {kind: Path(f"/tmp/zet-soak-{kind}-{tag}-{s.name}")
                          for kind in ("ready", "gate", "done")} for s in self.sessions}
        counts = {s.name: (s.regained, s.restored) for s in self.sessions}
        try:
            for s in self.sessions:
                p = paths[s.name]
                # Typed into the foreground of a shell whose producer runs in
                # the background: the burst waits for the gate, which opens
                # once the agent has no client, so that all of it overflows
                # the detached buffer and none is acknowledged.
                s.terminal.send(typed(
                    f"touch {p['ready']}; while [ ! -e {p['gate']} ]; do sleep 0.1; done; "
                    f"seq 1 {BURST_LINES}; touch {p['done']}"))
            self.pump_until(lambda: all(paths[s.name]["ready"].exists() for s in self.sessions),
                            "the shells to get the burst command")
            with ExitStack() as cuts:
                cut_of = {s.name: cuts.enter_context(link_cut(self.zet, s.terminal, self.iptables))
                          for s in self.sessions}
                self.pump_until(
                    lambda: all(cut_of[s.name].inode not in socket_inodes(s.terminal.pid)
                                and agent_detached(s, self.zet) for s in self.sessions),
                    "both ends to drop the cut connections")
                for s in self.sessions:
                    paths[s.name]["gate"].touch()
                self.pump_until(lambda: all(paths[s.name]["done"].exists() for s in self.sessions),
                                "the bursts to be written")
            self.pump_until(
                lambda: all(s.regained > counts[s.name][0] and s.restored > counts[s.name][1]
                            for s in self.sessions),
                f"cycle {number}: every client to reconnect and apply a snapshot")
            self.pump(SETTLE_SECONDS)
            taken = self.sample()
            for s in self.sessions:
                s.cycle_end_kib.append(taken[s.name])
        finally:
            for p in paths.values():
                for path in p.values():
                    path.unlink(missing_ok=True)


def snapshots_of(sessions: list[Session]) -> list[dict]:
    found = []
    for s in sessions:
        for line in s.log.read_text(errors="replace").splitlines():
            match = SNAPSHOT_LINE.search(line)
            if match:
                bytes_, history, flags = map(int, match.groups())
                found.append({"session": s.name, "bytes": bytes_, "scrollback_bytes": history,
                              "flags": flags})
    return found


def trend_kib(points: list[int]) -> float:
    """Least-squares growth over the whole run of `points`, in KiB."""
    n = len(points)
    mean_x, mean_y = (n - 1) / 2, sum(points) / n
    slope = (sum((x - mean_x) * (y - mean_y) for x, y in enumerate(points))
             / sum((x - mean_x) ** 2 for x in range(n)))
    return slope * (n - 1)


def write_summary(soak: Soak, constants: dict, snapshots: list[dict], cycles: int,
                  failure: str | None) -> None:
    sizes = [x["bytes"] for x in snapshots]
    summary = {
        "result": "fail" if failure else "pass", "failure": failure, "cycles": cycles,
        "seconds": round(soak.now()), "constants": constants, "factor": FACTOR,
        "bound_kib": soak.bound_kib,
        "sessions": {s.name: {"base_kib": s.base_kib, "peak_hwm_kib": s.hwm_kib,
                              "cycle_end_rss_kib": s.cycle_end_kib} for s in soak.sessions},
        "snapshots": {"count": len(sizes), "min": min(sizes, default=0), "max": max(sizes, default=0),
                      "mean": round(sum(sizes) / len(sizes)) if sizes else 0},
    }
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    with open(OUT / "snapshots.csv", "w", newline="") as file:
        writer = csv.DictWriter(file, ["session", "bytes", "scrollback_bytes", "flags"])
        writer.writeheader()
        writer.writerows(snapshots)
    lines = [f"### M6 soak: {summary['result']}", "",
             f"{cycles} forced path B cycles in {summary['seconds']} s; bound {soak.bound_kib} KiB "
             f"(N = {FACTOR:g}).", "", "| session | base KiB | peak HWM KiB |", "|---|---|---|"]
    lines += [f"| {s.name} | {s.base_kib} | {s.hwm_kib} |" for s in soak.sessions]
    lines += ["", f"Snapshots: {summary['snapshots']['count']}, bytes min/mean/max "
                  f"{summary['snapshots']['min']}/{summary['snapshots']['mean']}/{summary['snapshots']['max']}."]
    if failure:
        lines += ["", f"Failure: {failure}"]
    (OUT / "summary.md").write_text("\n".join(lines) + "\n")


def test_m6_soak(zet: Zet, clean_bashrc, iptables):
    scrollback = constant("src/grid/include/zet/grid/limits.hpp", "DEFAULT_SCROLLBACK_LIMIT")
    disconnect = constant("src/session/include/zet/session/limits.hpp", "DISCONNECT_BUFFER_MAX")
    snapshot_max = constant("src/wire/include/zet/wire/limits.hpp", "SNAPSHOT_MAX_BYTES")
    cell = cell_size()
    # Everything but base_agent, in KiB; the burst must really exceed it.
    fixed = (2 * COLS * ROWS * cell + scrollback * COLS * cell + disconnect + snapshot_max) // KIB
    assert BURST_LINES * 7 > disconnect, "the burst does not exceed the detached buffer"
    assert SOAK_SECONDS >= CYCLE_SECONDS, "a run shorter than one cycle forces no path B"
    constants = {"cols": COLS, "rows": ROWS, "sizeof_Cell": cell, "scrollback_limit": scrollback,
                 "disconnect_buffer_max": disconnect, "snapshot_max_bytes": snapshot_max,
                 "fixed_kib": fixed}

    OUT.mkdir(parents=True, exist_ok=True)
    OUT.chmod(0o777)  # the agents, running as the test user, log into it
    sessions: list[Session] = []
    soak: Soak | None = None
    cycles = 0
    failure: str | None = None
    try:
        for name in PRODUCERS:
            log = OUT / f"agent-{name}.log"
            terminal = zet.start(ZET_AGENT_LOG=str(log))
            terminal.expect(PROMPT)
            agent = parent_of(shell_pid(terminal))
            sessions.append(Session(name, terminal, agent, log))
        for s in sessions:
            s.base_kib = proc_kib(s.agent, "VmRSS")
        # One bound for all three: the largest base, since they are one binary.
        soak = Soak(zet, iptables, sessions, int(FACTOR * (max(s.base_kib for s in sessions) + fixed)))
        for s in sessions:
            s.terminal.send(typed(f"({PRODUCERS[s.name]}) &"))
        soak.pump(SAMPLE_SECONDS)  # the producers' own steady state, first

        next_cycle = soak.now() + CYCLE_SECONDS
        while soak.now() < SOAK_SECONDS:
            soak.pump(1.0)
            if soak.now() >= next_cycle:
                cycles += 1
                soak.cycle(cycles)
                next_cycle += CYCLE_SECONDS
        assert cycles >= 1
        soak.sample()
        for s in sessions:
            points = s.cycle_end_kib[WARMUP_CYCLES:]
            if len(points) >= MIN_TREND_POINTS:
                growth = trend_kib(points)
                assert growth <= TREND_SLACK_KIB, (
                    f"session {s.name}: RSS at the end of cycles grows {growth:.0f} KiB over "
                    f"{len(points)} cycles: {points}")
        assert snapshots_of(sessions), "the agents logged no snapshot"
    except BaseException as error:
        failure = f"{type(error).__name__}: {error}"
        raise
    finally:
        if soak is not None:
            soak.csv_file.close()
            write_summary(soak, constants, snapshots_of(sessions), cycles, failure)
        for s in sessions:
            s.terminal.kill()
