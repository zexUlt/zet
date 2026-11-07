"""The M6 soak (design.md §16): three sessions with different output rates and
screen contents, a forced path B every cycle, and the agents' RSS held against
a per-agent bound and against growth from cycle to cycle.

Not collected by the normal run: the name does not start with `test_`, and the
workflow .github/workflows/m6-soak.yml names the file. Two phases, two tests,
each switched off by a duration of 0. The settings come from the environment
(tools/integration.sh forwards every ZET_SOAK_*):

    ZET_SOAK_SECONDS             main phase, whole run              default 14400
    ZET_SOAK_CYCLE_SECONDS       main phase, between two cuts       default 1800
    ZET_SOAK_FAST_SECONDS        fast phase, whole run              default 600
    ZET_SOAK_FAST_CYCLE_SECONDS  fast phase, between two cuts       default 25
    ZET_SOAK_SAMPLE_SECONDS      between two RSS samples            default 30
    ZET_SOAK_OUT                 where the series and summary go    default build/soak-m6

Main phase: a cycle cuts all three links, has each shell print more than the
agent's detached buffer holds (ZET_DISCONNECT_BUFFER_MAX is left at its
default, so the ring really fills to 16 MiB), lifts the cuts and waits for
every client to apply a snapshot. It is the realistic load, and with its few
cycles it sees only a large per-cycle leak.

Fast phase: the same cycle against a 64 KiB detached buffer and a 300 KB burst,
every 25 s, so that a leak of one snapshot or one reconnect (design.md ~998,
"idInfoMap never cleared") adds up over hundreds of cycles.

The three sessions differ in what the screen holds, for the compression
decision of §18: `plain` is `seq` output at 80x24 (the best case), `ansi` is
`ls --color`-like 16 and 256 colour lines at 80x24 (typical), `truecolor` is
one SGR per cell at 220x50 (the worst case: it overruns SNAPSHOT_MAX_BYTES and
walks the degrade ladder).

Output, under ZET_SOAK_OUT/<phase>: rss.csv (a sample per session per
interval), snapshots.csv (one row per snapshot, from the agents' own log
lines), compression.csv (the bytes the client received for a snapshot and
their zlib size), and summary.json / summary.md.
"""

from __future__ import annotations

import csv
import json
import os
import re
import statistics
import time
import uuid
import zlib
from contextlib import ExitStack
from dataclasses import dataclass, field
from pathlib import Path

import pytest

from e2e import PROMPT, REGAINED_NOTICE, LOST_NOTICE, Terminal, Zet, connection_to_agent, parent_of, \
    socket_inodes, sockets_of, TCP_ESTABLISHED
from harness import PtyProcess
from m6_support import SNAPSHOT_NOTICE, SNAPSHOT_PREAMBLE, link_cut, shell_pid, typed

REPO = Path(__file__).resolve().parents[2]

SOAK_SECONDS = int(os.environ.get("ZET_SOAK_SECONDS", 4 * 3600))
CYCLE_SECONDS = int(os.environ.get("ZET_SOAK_CYCLE_SECONDS", 30 * 60))
FAST_SECONDS = int(os.environ.get("ZET_SOAK_FAST_SECONDS", 600))
FAST_CYCLE_SECONDS = int(os.environ.get("ZET_SOAK_FAST_CYCLE_SECONDS", 25))
SAMPLE_SECONDS = int(os.environ.get("ZET_SOAK_SAMPLE_SECONDS", 30))
OUT = Path(os.environ.get("ZET_SOAK_OUT", "build/soak-m6")).resolve()

KIB = 1024
MIB = 1024 * KIB

# `seq 1 3000000` is 22.8 MB: more than DISCONNECT_BUFFER_MAX (16 MiB). The
# fast phase's `seq 1 50000` is 289 KB against its 64 KiB buffer.
BURST_LINES = 3_000_000
FAST_BURST_LINES = 50_000
FAST_BURST_BYTES = 300_000
# The detached buffer of the fast phase. The producers there are slow (under
# 12 KB/s): at the cut, the output the client has not acknowledged yet is
# still held, and if it exceeds this ceiling the agent stops reading the shell
# without ever flagging an overflow (a state the compiled 16 MiB buffer, four
# times the 4 MiB of unacknowledged output, cannot reach, and only the hook can).
FAST_DISCONNECT_BUFFER = 64 * KIB

# What the bound allows on top of the screens, the scrollback and two
# snapshots: the allocator's slack, the pty and socket buffers, stacks.
HEADROOM_KIB = 1 * KIB


@dataclass(frozen=True)
class Profile:
    """One kind of session: its terminal's size, the background producer (bytes
    per second, roughly 600 000, 120 and 30) and what the burst prints."""
    name: str
    rows: int
    cols: int
    producer: str
    fast_producer: str  # the fast phase's, slower: see FAST_DISCONNECT_BUFFER
    burst: str  # "seq", "ansi" or "truecolor"


PROFILES = [
    Profile("plain", 24, 80, "while :; do seq 1 5000; sleep 0.05; done",
            "while :; do seq 1 300; sleep 0.1; done", "seq"),
    # The colour sessions print little between a burst and the snapshot: a
    # producer that scrolls thousands of lines a second would replace the
    # colour in the scrollback with its own plain text.
    Profile("ansi", 24, 80, "while :; do date; sleep 0.5; done",
            "while :; do date; sleep 0.5; done", "ansi"),
    Profile("truecolor", 50, 220, "while :; do date; sleep 1; done",
            "while :; do date; sleep 1; done", "truecolor"),
]

# Prints `total` bytes of lines of `cols` cells and stops. `truecolor`: one SGR
# per cell, the colour changing from cell to cell and line to line. `ansi`: the
# words of an `ls --color` listing, in the 16 and the 256 colour palettes.
BURST_SCRIPT = r"""
import sys
mode, total, cols = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
words = [("\x1b[01;34m", "src"), ("\x1b[0m", "README.md"), ("\x1b[01;32m", "build.sh"),
         ("\x1b[38;5;208m", "archive.tar.gz"), ("\x1b[01;36m", "link"), ("\x1b[0;31m", "core.dump"),
         ("\x1b[38;5;33m", "docs"), ("\x1b[38;5;70m", "notes.txt")]
out, sent, i = sys.stdout.buffer, 0, 0
while sent < total:
    if mode == "truecolor":
        line = "".join(f"\x1b[38;2;{(i * 37 + x * 5) % 256};{(i * 11 + x * 3) % 256};{(x * 7 + i) % 256}m\u2588"
                       for x in range(cols)) + "\x1b[0m\n"
    else:
        line = "".join(f"{c}{n}{(i * 7 + k * 13) % 997}\x1b[0m" + " " * (18 - len(n) - len(str((i * 7 + k * 13) % 997)))
                       for k, (c, n) in enumerate(words[(i + k * 3) % len(words)] for k in range(cols // 18))) + "\n"
    data = line.encode()
    out.write(data)
    sent += len(data)
    i += 1
out.flush()
"""

# A cycle that takes longer than this to recover is a hang, not a slow run.
CYCLE_DEADLINE_SECONDS = 300
# The suite's default of 20 s reaps an agent whose burst takes longer than that
# to write on a loaded runner; here a cut is as long as CYCLE_DEADLINE_SECONDS.
ORPHAN_TIMEOUT_SECONDS = 2 * CYCLE_DEADLINE_SECONDS
# After the last session has recovered: let the agent settle before the sample
# that stands for the cycle.
SETTLE_SECONDS = 5

# Growth is judged per session on the cycle-end RSS samples: the median of the
# last GROWTH_WINDOW points against the median of the first GROWTH_WINDOW after
# WARMUP_CYCLES warm-up cycles. A leak of the idInfoMap class grows with every
# reconnect; allocator noise does not.
WARMUP_CYCLES = 2
GROWTH_WINDOW = 3
MIN_CYCLES = WARMUP_CYCLES + 2 * GROWTH_WINDOW
# The first value, from the noise floor measured on a baseline: the cycle-end
# RSS stayed within +-8 KiB over eight cycles. Recalibrate from the rss.csv of
# the first four-hour CI run: set it to a small multiple of the largest
# window difference a clean run shows.
GROWTH_THRESHOLD_KIB = 128

SNAPSHOT_FIELDS = ("bytes", "scrollback_bytes", "flags", "rows", "cols", "degradation")
SNAPSHOT_LINE = re.compile(r"grid snapshot supplied (.*)$")


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


def parse_snapshot_line(line: str) -> dict | None:
    match = SNAPSHOT_LINE.search(line)
    if not match:
        return None
    found = dict(pair.split("=", 1) for pair in match.group(1).split())
    return {name: int(found[name]) for name in SNAPSHOT_FIELDS}


@dataclass
class Session:
    profile: Profile
    terminal: Terminal
    agent: int
    log: Path
    base_kib: int = 0
    budget_kib: int = 0
    regained: int = 0
    restored: int = 0
    hwm_kib: int = 0
    cycle_end_kib: list[int] = field(default_factory=list)
    # The agent's own snapshot lines, read incrementally.
    snapshots: list[dict] = field(default_factory=list)
    log_offset: int = 0
    # Everything the client wrote since `capture` was set, for the compression
    # ratio of the snapshot in it.
    capture: bytearray | None = None
    compression: list[dict] = field(default_factory=list)

    @property
    def name(self) -> str:
        return self.profile.name

    @property
    def screens_kib(self) -> int:
        """What a session of this size holds apart from the replay ring and
        the snapshot: both screens and a full scrollback."""
        cell = cell_size()
        scrollback = constant("src/grid/include/zet/grid/limits.hpp", "DEFAULT_SCROLLBACK_LIMIT")
        p = self.profile
        return (2 * p.rows * p.cols * cell + scrollback * p.cols * cell) // KIB

    def read_log(self) -> None:
        with open(self.log, "rb") as file:
            file.seek(self.log_offset)
            data = file.read()
        complete = data[:data.rfind(b"\n") + 1]
        self.log_offset += len(complete)
        for line in complete.decode(errors="replace").splitlines():
            parsed = parse_snapshot_line(line)
            if parsed:
                self.snapshots.append(parsed)

    @property
    def bound_kib(self) -> int:
        """The most the agent's RSS may be: what it held after the handshake
        (which includes the replay ring, already touched), both screens, a
        full scrollback, two snapshots of the largest size seen (the one in
        flight and the one being replaced) and the headroom. One agent, so
        N = 1."""
        biggest = max((x["bytes"] for x in self.snapshots), default=0)
        return self.base_kib + self.screens_kib + 2 * biggest // KIB + HEADROOM_KIB

    def read(self) -> None:
        """Takes what the terminal has, counts zet's notices in it, and keeps
        the buffer from growing without bound: nothing here reads it."""
        before = len(self.terminal.buffer)
        got = self.terminal.read_more(0)
        if got == 0:
            raise AssertionError(f"session {self.name} ended: {self.terminal.tail()!r}")
        if self.capture is not None:
            self.capture += self.terminal.buffer[before:]
        for notice, attribute in ((REGAINED_NOTICE, "regained"), (SNAPSHOT_NOTICE, "restored")):
            start = max(0, before - len(notice) + 1)
            setattr(self, attribute,
                    getattr(self, attribute) + self.terminal.buffer.count(notice, start))
        if len(self.terminal.buffer) > 1024 * KIB:
            del self.terminal.buffer[:-4 * KIB]
            self.terminal.consumed = 0

    def take_snapshot_bytes(self, number: int) -> None:
        """From what the client wrote since the cut: the snapshot, which opens
        with the preamble and is followed by zet's notice that it was applied.
        Records its size and its size under zlib level 6, which is what a
        compressed snapshot would cost. Needs no hook in the agent."""
        captured, self.capture = bytes(self.capture or b""), None
        start = captured.rfind(SNAPSHOT_PREAMBLE)
        end = captured.find(SNAPSHOT_NOTICE, start)
        if start < 0 or end < 0:
            self.compression.append({"cycle": number, "client_bytes": 0, "zlib_bytes": 0})
            return
        raw = captured[start:end]
        for notice in (LOST_NOTICE, REGAINED_NOTICE):
            raw = raw.replace(notice, b"")
        self.compression.append({"cycle": number, "client_bytes": len(raw),
                                 "zlib_bytes": len(zlib.compress(raw, 6))})


def agent_detached(session: Session, zet: Zet) -> bool:
    return not any(s.state == TCP_ESTABLISHED for s in sockets_of(session.agent, zet.host.user))


class Soak:
    def __init__(self, phase: "Phase", zet: Zet, iptables: str, sessions: list[Session],
                 burst_script: Path) -> None:
        self.phase = phase
        self.zet = zet
        self.iptables = iptables
        self.sessions = sessions
        self.burst_script = burst_script
        self.started = time.monotonic()
        self.next_sample = self.started
        self.out = OUT / phase.name
        self.out.mkdir(parents=True, exist_ok=True)
        self.csv_file = open(self.out / "rss.csv", "w", newline="")
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
                what = what() if callable(what) else what
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
            s.read_log()
            rss, hwm = proc_kib(s.agent, "VmRSS"), proc_kib(s.agent, "VmHWM")
            s.hwm_kib = hwm
            taken[s.name] = rss
            bound = s.bound_kib
            self.csv.writerow([f"{self.now():.0f}", s.name, rss, hwm, bound])
            # HWM - base <= the budget (screens, scrollback, two snapshots,
            # headroom) is the same inequality as HWM <= bound.
            assert hwm <= bound, (
                f"session {s.name}: peak RSS {hwm} KiB exceeds the bound {bound} KiB "
                f"(base {s.base_kib} KiB, budget {bound - s.base_kib} KiB, "
                f"grew {hwm - s.base_kib} KiB) at t={self.now():.0f}s")
        self.csv_file.flush()
        return taken

    def burst_command(self, s: Session) -> str:
        if s.profile.burst == "seq":
            lines = BURST_LINES if self.phase.default_buffer else FAST_BURST_LINES
            return f"seq 1 {lines}"
        total = self.phase.ansi_bytes
        return f"python3 {self.burst_script} {s.profile.burst} {total} {s.profile.cols}"

    def prefill(self, scrollback: int) -> None:
        """Fills each grid, screen and scrollback, with the session's own kind
        of output while the link is up, so that the cycles start from the
        steady state. Without it the RSS of a session that prints colour rises
        for as long as its scrollback is filling, which a small burst per cycle
        makes hundreds of cycles, and a leak is not told from that."""
        tag = uuid.uuid4().hex
        done = {s.name: Path(f"/tmp/zet-soak-prefill-{tag}-{s.name}") for s in self.sessions}
        try:
            for s in self.sessions:
                lines = scrollback + 2 * s.profile.rows + 50
                if s.profile.burst == "seq":
                    command = f"seq 1 {lines}"
                else:
                    # Lower bounds of a line's size, so that it is lines or more.
                    per_line = 18 * s.profile.cols if s.profile.burst == "truecolor" else 100
                    command = (f"python3 {self.burst_script} {s.profile.burst} "
                               f"{lines * per_line} {s.profile.cols}")
                s.terminal.send(typed(f"{command}; touch {done[s.name]}"))
            self.pump_until(lambda: all(p.exists() for p in done.values()),
                            "the shells to print the prefill")
        finally:
            for path in done.values():
                path.unlink(missing_ok=True)

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
                    f"{self.burst_command(s)}; touch {p['done']}"))
            self.pump_until(lambda: all(paths[s.name]["ready"].exists() for s in self.sessions),
                            "the shells to get the burst command")
            for s in self.sessions:
                s.capture = bytearray()
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
                                lambda: "the bursts to be written (missing: " + ", ".join(
                                    s.name for s in self.sessions if not paths[s.name]["done"].exists()) + ")")
            self.pump_until(
                lambda: all(s.regained > counts[s.name][0] and s.restored > counts[s.name][1]
                            for s in self.sessions),
                f"cycle {number}: every client to reconnect and apply a snapshot")
            for s in self.sessions:
                s.take_snapshot_bytes(number)
            self.pump(SETTLE_SECONDS)
            taken = self.sample()
            for s in self.sessions:
                s.cycle_end_kib.append(taken[s.name])
        finally:
            for s in self.sessions:
                s.capture = None
            for p in paths.values():
                for path in p.values():
                    path.unlink(missing_ok=True)


@dataclass(frozen=True)
class Phase:
    name: str
    seconds: int
    cycle_seconds: int
    # None: the agent's own default of 16 MiB, the real thing.
    disconnect_buffer: int | None
    ansi_bytes: int

    @property
    def default_buffer(self) -> bool:
        return self.disconnect_buffer is None

    @property
    def cycles(self) -> int:
        """How many cycles the run completes: the loop does not end before."""
        return self.seconds // self.cycle_seconds


def growth_kib(points: list[int]) -> int:
    """The median of the last GROWTH_WINDOW cycle-end samples less the median
    of the first GROWTH_WINDOW after the warm-up, in KiB."""
    assert len(points) >= WARMUP_CYCLES + 2 * GROWTH_WINDOW, (
        f"{len(points)} cycle-end samples, {WARMUP_CYCLES + 2 * GROWTH_WINDOW} needed "
        f"({WARMUP_CYCLES} warm-up and two windows of {GROWTH_WINDOW})")
    points = points[WARMUP_CYCLES:]
    return round(statistics.median(points[-GROWTH_WINDOW:]) - statistics.median(points[:GROWTH_WINDOW]))


def write_summary(soak: Soak, constants: dict, cycles: int, failure: str | None) -> None:
    sizes = [x["bytes"] for s in soak.sessions for x in s.snapshots]
    summary = {
        "result": "fail" if failure else "pass", "failure": failure, "phase": soak.phase.name,
        "cycles": cycles, "seconds": round(soak.now()), "constants": constants,
        "growth_threshold_kib": GROWTH_THRESHOLD_KIB,
        "sessions": {}, "snapshots": {"count": len(sizes), "min": min(sizes, default=0),
                                      "max": max(sizes, default=0),
                                      "mean": round(sum(sizes) / len(sizes)) if sizes else 0},
    }
    lines = [f"### M6 soak, {soak.phase.name} phase: {summary['result']}", "",
             f"{cycles} forced path B cycles in {summary['seconds']} s; growth threshold "
             f"{GROWTH_THRESHOLD_KIB} KiB.", "",
             "| session | size | base KiB | peak HWM KiB | bound KiB | growth KiB | snapshots | "
             "snapshot bytes max | degradation max | zlib ratio mean |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    with open(soak.out / "snapshots.csv", "w", newline="") as file:
        writer = csv.DictWriter(file, ["session", *SNAPSHOT_FIELDS])
        writer.writeheader()
        for s in soak.sessions:
            writer.writerows({"session": s.name, **x} for x in s.snapshots)
    with open(soak.out / "compression.csv", "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["session", "cycle", "client_bytes", "zlib_level6_bytes", "ratio"])
        for s in soak.sessions:
            for c in s.compression:
                ratio = c["zlib_bytes"] / c["client_bytes"] if c["client_bytes"] else 0
                writer.writerow([s.name, c["cycle"], c["client_bytes"], c["zlib_bytes"], f"{ratio:.4f}"])
    for s in soak.sessions:
        ratios = [c["zlib_bytes"] / c["client_bytes"] for c in s.compression if c["client_bytes"]]
        growth = growth_kib(s.cycle_end_kib) if len(s.cycle_end_kib) >= MIN_CYCLES else None
        degradation = max((x["degradation"] for x in s.snapshots), default=0)
        biggest = max((x["bytes"] for x in s.snapshots), default=0)
        mean_ratio = statistics.fmean(ratios) if ratios else 0
        summary["sessions"][s.name] = {
            "rows": s.profile.rows, "cols": s.profile.cols, "base_kib": s.base_kib,
            "peak_hwm_kib": s.hwm_kib, "bound_kib": s.bound_kib, "growth_kib": growth,
            "snapshots": len(s.snapshots), "snapshot_bytes_max": biggest,
            "degradation_max": degradation, "zlib_ratio_mean": round(mean_ratio, 4),
            "cycle_end_rss_kib": s.cycle_end_kib}
        lines.append(f"| {s.name} | {s.profile.rows}x{s.profile.cols} | {s.base_kib} | {s.hwm_kib} | "
                     f"{s.bound_kib} | {growth} | {len(s.snapshots)} | {biggest} | {degradation} | "
                     f"{mean_ratio:.3f} |")
    (soak.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if failure:
        lines += ["", f"Failure: {failure}"]
    (soak.out / "summary.md").write_text("\n".join(lines) + "\n")


def start_session(zet: Zet, profile: Profile, **agent_env: str) -> Terminal:
    """zet.start, on a pty of the profile's size."""
    return Terminal(PtyProcess(zet.argv(zet.agent_env(**agent_env)), env=zet.env(),
                               rows=profile.rows, cols=profile.cols))


def run_phase(zet: Zet, iptables: str, phase: Phase) -> None:
    if phase.seconds == 0:
        pytest.skip(f"the {phase.name} phase is switched off (duration 0)")
    scrollback = constant("src/grid/include/zet/grid/limits.hpp", "DEFAULT_SCROLLBACK_LIMIT")
    disconnect = constant("src/session/include/zet/session/limits.hpp", "DISCONNECT_BUFFER_MAX")
    snapshot_max = constant("src/wire/include/zet/wire/limits.hpp", "SNAPSHOT_MAX_BYTES")
    ring = phase.disconnect_buffer or disconnect
    assert BURST_LINES * 7 > disconnect and phase.ansi_bytes > ring, \
        "the burst does not exceed the detached buffer"
    assert FAST_BURST_LINES * 5 > FAST_DISCONNECT_BUFFER, "the fast burst does not exceed its buffer"
    assert phase.cycle_seconds > 0 and phase.seconds >= phase.cycle_seconds, \
        "a run shorter than one cycle forces no path B"
    assert phase.cycles >= MIN_CYCLES, (
        f"{phase.seconds} s of {phase.cycle_seconds} s cycles is {phase.cycles} cycles; "
        f"the growth check needs {MIN_CYCLES}")
    constants = {"sizeof_Cell": cell_size(), "scrollback_limit": scrollback,
                 "disconnect_buffer_max": ring, "snapshot_max_bytes": snapshot_max,
                 "headroom_kib": HEADROOM_KIB}

    OUT.mkdir(parents=True, exist_ok=True)
    OUT.chmod(0o777)  # the agents, running as the test user, log into it
    script = Path(f"/tmp/zet-soak-burst-{uuid.uuid4().hex}.py")
    script.write_text(BURST_SCRIPT)
    script.chmod(0o644)
    sessions: list[Session] = []
    soak: Soak | None = None
    cycles = 0
    failure: str | None = None
    try:
        for profile in PROFILES:
            log = OUT / f"agent-{phase.name}-{profile.name}.log"
            log.unlink(missing_ok=True)  # the agent appends: a stale file would be counted
            env = {"ZET_AGENT_LOG": str(log), "ZET_ORPHAN_TIMEOUT": str(ORPHAN_TIMEOUT_SECONDS)}
            if phase.disconnect_buffer is not None:
                env["ZET_DISCONNECT_BUFFER_MAX"] = str(phase.disconnect_buffer)
            terminal = start_session(zet, profile, **env)
            terminal.expect(PROMPT)
            agent = parent_of(shell_pid(terminal))
            sessions.append(Session(profile, terminal, agent, log))
        for s in sessions:
            s.base_kib = proc_kib(s.agent, "VmRSS")
        soak = Soak(phase, zet, iptables, sessions, script)
        for s in sessions:
            producer = s.profile.producer if phase.default_buffer else s.profile.fast_producer
            s.terminal.send(typed(f"({producer}) &"))
        soak.prefill(scrollback)
        soak.pump(SAMPLE_SECONDS)  # the producers' own steady state, first

        next_cycle = soak.now() + phase.cycle_seconds
        while cycles < phase.cycles:
            soak.pump(1.0)
            if soak.now() >= next_cycle:
                cycles += 1
                soak.cycle(cycles)
                next_cycle += phase.cycle_seconds
        soak.sample()
        for s in sessions:
            growth = growth_kib(s.cycle_end_kib)
            assert growth <= GROWTH_THRESHOLD_KIB, (
                f"session {s.name}: cycle-end RSS grew {growth} KiB between the first and the last "
                f"{GROWTH_WINDOW} points after {WARMUP_CYCLES} warm-up cycles "
                f"(threshold {GROWTH_THRESHOLD_KIB} KiB): {s.cycle_end_kib}")
            assert len(s.snapshots) >= cycles, (
                f"session {s.name}: {len(s.snapshots)} snapshots logged for {cycles} cycles")
    except BaseException as error:
        failure = f"{type(error).__name__}: {error}"
        raise
    finally:
        script.unlink(missing_ok=True)
        if soak is not None:
            soak.csv_file.close()
            for s in sessions:
                s.read_log()
            write_summary(soak, constants, cycles, failure)
        for s in sessions:
            s.terminal.kill()


def test_m6_soak(zet: Zet, clean_bashrc, iptables):
    """The realistic load: the default 16 MiB replay ring, filled each cycle."""
    disconnect = constant("src/session/include/zet/session/limits.hpp", "DISCONNECT_BUFFER_MAX")
    run_phase(zet, iptables,
              Phase("main", SOAK_SECONDS, CYCLE_SECONDS, None, disconnect + 4 * MIB))


def test_m6_soak_fast(zet: Zet, clean_bashrc, iptables):
    """Many cheap cycles, for a leak of one snapshot or one reconnect."""
    run_phase(zet, iptables,
              Phase("fast", FAST_SECONDS, FAST_CYCLE_SECONDS, FAST_DISCONNECT_BUFFER, FAST_BURST_BYTES))
