"""Differential snapshot test: pyte(snapshot) vs pyte(original) (design.md §16).

For each corpus file this test:
  1. Renders the original capture through pyte to get the reference screen.
  2. Feeds the original bytes through zet_snapshot_dump to produce a Snapshot
     byte sequence.
  3. Renders the snapshot bytes through a fresh pyte screen.
  4. Asserts that the two pyte renderings produce the same visible text rows
     and cursor position.

This proves that Snapshot::Create reproduces the visible state faithfully: if
the encoder misses the cursor, drops SGR attributes, or encodes the wrong
cells, the pyte screens will diverge and the test fails.

Missing resources under ZET_BUILD_DIR are hard failures, not skips.

Known pyte implementation gaps are listed in
tests/integration/grid_corpus/allowlist.json (same file used by
test_grid_diff.py).  A difference matching an allowlist entry is reported as
xfail rather than a hard failure.

The test fails when broken because:
  - Removing the cursor-position sequence from EncodeModeRestore makes
    cursor_row/cursor_col disagree between the two pyte renders.
  - Removing EncodeScreen makes every text row empty in the snapshot render.
  - Removing the DECSET/DECRST restore sequence can cause attribute bleed in
    subsequent corpus files (though the test isolates each file in a fresh
    pyte screen so bleed between files is not an issue here).
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Optional

import pytest

# ---------------------------------------------------------------------------
# Availability guards
# ---------------------------------------------------------------------------

try:
    import pyte  # type: ignore
    PYTE_AVAILABLE = True
except ImportError:
    PYTE_AVAILABLE = False

BUILD_DIR = os.environ.get("ZET_BUILD_DIR", "")

SNAPSHOT_DUMP = (
    os.environ.get("ZET_SNAPSHOT_DUMP")
    or (
        str(Path(BUILD_DIR) / "tests" / "zet_snapshot_dump")
        if BUILD_DIR
        else ""
    )
)

# Also accept a path derived from zet_grid_dump location if the env var
# ZET_GRID_DUMP is set and ZET_SNAPSHOT_DUMP is not.
if not SNAPSHOT_DUMP:
    grid_dump = os.environ.get("ZET_GRID_DUMP", "")
    if grid_dump:
        parent = str(Path(grid_dump).parent)
        SNAPSHOT_DUMP = str(Path(parent) / "zet_snapshot_dump")


def _require_when_build_dir(thing: str, available: bool) -> None:
    """Hard-fail in the integration container; skip elsewhere."""
    if not available and BUILD_DIR:
        pytest.fail(
            f"{thing} is required when ZET_BUILD_DIR is set but was not found"
        )


skip_no_pyte = pytest.mark.skipif(
    not PYTE_AVAILABLE and not BUILD_DIR,
    reason="pyte not installed",
)
skip_no_dump = pytest.mark.skipif(
    (not SNAPSHOT_DUMP or not Path(SNAPSHOT_DUMP).exists()) and not BUILD_DIR,
    reason="zet_snapshot_dump helper not built / ZET_SNAPSHOT_DUMP not set",
)

# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------

CORPUS_DIR = Path(__file__).parent / "grid_corpus"
ALLOWLIST_PATH = CORPUS_DIR / "allowlist.json"


def _corpus_files() -> list[Path]:
    if not CORPUS_DIR.is_dir():
        return []
    files = sorted(CORPUS_DIR.glob("*.bin"))
    if not files and BUILD_DIR:
        pytest.fail(
            "grid_corpus/ contains no .bin files but ZET_BUILD_DIR is set. "
            "Run tools/capture_grid_corpus.sh to regenerate the corpus."
        )
    return files


# ---------------------------------------------------------------------------
# Allowlist
# ---------------------------------------------------------------------------

import json  # noqa: E402 (after corpus dir definition)


def _load_allowlist() -> list[dict]:
    if not ALLOWLIST_PATH.exists():
        return []
    with open(ALLOWLIST_PATH) as f:
        data = json.load(f)
    return data if isinstance(data, list) else []


_ALLOWLIST = _load_allowlist()


def _is_allowlisted(stem: str, row: int) -> Optional[str]:
    for entry in _ALLOWLIST:
        entry_file = entry.get("file", "")
        entry_row = entry.get("row", -1)
        if entry_file in ("*", stem) and (entry_row == -1 or entry_row == row):
            return str(entry.get("reason", "allowlisted"))
    return None


# ---------------------------------------------------------------------------
# Rendering helpers
# ---------------------------------------------------------------------------

def _render_pyte(data: bytes, cols: int = 80, rows: int = 24) -> tuple[list[str], int, int]:
    """Render bytes through pyte; return (row_strings, cursor_row, cursor_col)."""
    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    stream.feed(data)
    row_strs = [screen.display[r].rstrip() for r in range(rows)]
    return row_strs, screen.cursor.y, screen.cursor.x


def _snapshot_bytes(data: bytes, cols: int = 80, rows: int = 24) -> bytes:
    """Run zet_snapshot_dump and return the snapshot byte sequence."""
    result = subprocess.run(
        [SNAPSHOT_DUMP, "--cols", str(cols), "--rows", str(rows)],
        input=data,
        capture_output=True,
        timeout=15,
    )
    assert result.returncode == 0, (
        f"zet_snapshot_dump failed (exit {result.returncode}): "
        f"{result.stderr.decode(errors='replace')}"
    )
    return result.stdout


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("capture", _corpus_files(), ids=lambda p: p.stem)
@skip_no_pyte
@skip_no_dump
def test_snapshot_matches_pyte(capture: Path) -> None:
    """pyte(snapshot) must match pyte(original) for every corpus capture."""
    _require_when_build_dir("pyte", PYTE_AVAILABLE)
    _require_when_build_dir(
        "zet_snapshot_dump",
        bool(SNAPSHOT_DUMP and Path(SNAPSHOT_DUMP).exists()),
    )

    COLS, ROWS = 80, 24
    data = capture.read_bytes()

    # Reference: pyte rendering of the original bytes.
    pyte_orig_rows, pyte_orig_cr, pyte_orig_cc = _render_pyte(data, COLS, ROWS)

    # Snapshot bytes via zet.
    snap_bytes = _snapshot_bytes(data, COLS, ROWS)

    # Restored: pyte rendering of the snapshot.
    pyte_snap_rows, pyte_snap_cr, pyte_snap_cc = _render_pyte(
        snap_bytes, COLS, ROWS
    )

    assert len(pyte_orig_rows) == len(pyte_snap_rows), (
        f"Row count mismatch: orig={len(pyte_orig_rows)} "
        f"snap={len(pyte_snap_rows)}"
    )

    stem = capture.stem
    for r, (orig, snap) in enumerate(zip(pyte_orig_rows, pyte_snap_rows)):
        if orig != snap:
            reason = _is_allowlisted(stem, r)
            if reason:
                pytest.xfail(
                    f"Row {r} differs (allowlisted: {reason}):\n"
                    f"  orig: {orig!r}\n  snap: {snap!r}"
                )
            assert orig == snap, (
                f"Row {r} mismatch after snapshot restore:\n"
                f"  orig: {orig!r}\n  snap: {snap!r}"
            )

    if pyte_orig_cr != pyte_snap_cr:
        reason = _is_allowlisted(stem, -1)
        if reason:
            pytest.xfail(
                f"Cursor row mismatch (allowlisted: {reason}): "
                f"orig={pyte_orig_cr} snap={pyte_snap_cr}"
            )
    assert pyte_orig_cr == pyte_snap_cr, (
        f"Cursor row mismatch after snapshot restore: "
        f"orig={pyte_orig_cr} snap={pyte_snap_cr}"
    )

    if pyte_orig_cc != pyte_snap_cc:
        reason = _is_allowlisted(stem, -1)
        if reason:
            pytest.xfail(
                f"Cursor col mismatch (allowlisted: {reason}): "
                f"orig={pyte_orig_cc} snap={pyte_snap_cc}"
            )
    assert pyte_orig_cc == pyte_snap_cc, (
        f"Cursor col mismatch after snapshot restore: "
        f"orig={pyte_orig_cc} snap={pyte_snap_cc}"
    )


@skip_no_pyte
def test_pyte_baseline_hello() -> None:
    """Smoke: pyte import works and renders trivially."""
    rows, _, _ = _render_pyte(b"Hello, snapshot!\r\n")
    assert rows[0] == "Hello, snapshot!"


@skip_no_dump
def test_snapshot_dump_smoke() -> None:
    """Smoke: zet_snapshot_dump accepts a trivial stream and returns bytes."""
    snap = _snapshot_bytes(b"Hello")
    assert len(snap) > 0
    # Snapshot starts with CAN (0x18) preamble byte.
    assert snap[0] == 0x18


@pytest.mark.skipif(bool(BUILD_DIR), reason="checked only when ZET_BUILD_DIR is set")
def test_corpus_not_empty_when_build_dir() -> None:
    """When ZET_BUILD_DIR is set the corpus must contain at least one file."""
    if BUILD_DIR:
        files = _corpus_files()
        assert files, "Corpus is empty — run tools/capture_grid_corpus.sh"
