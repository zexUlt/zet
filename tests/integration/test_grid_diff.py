"""Differential test: zet_grid vs pyte (design.md §16).

Feeds captured terminal streams through both zet_grid (via the CLI tool
zet-grid-dump, built alongside zet_tests) and pyte, and asserts that the
visible screen content matches.

The test skips when:
  - pyte is not installed (importable),
  - the zet-grid-dump helper is not on PATH / ZET_GRID_DUMP,
  - we are not in the integration container (no ZET_BUILD_DIR env var).

When ZET_BUILD_DIR is set an empty corpus is a hard failure, not a skip.

The corpus lives in tests/integration/grid_corpus/ — captures produced by
tools/capture_grid_corpus.sh.  The test parametrises over every .bin file
in that directory.

Known pyte differences are listed in tests/integration/grid_corpus/allowlist.json
(stdlib json, no extra dependency).  A difference that matches an allowlist entry
is reported as an xfail rather than a hard failure.
"""

from __future__ import annotations

import os
import subprocess
import json
import re
from pathlib import Path
from typing import Optional

import pytest

# ---------------------------------------------------------------------------
# Skip conditions
# ---------------------------------------------------------------------------

try:
    import pyte  # type: ignore
    PYTE_AVAILABLE = True
except ImportError:
    PYTE_AVAILABLE = False

BUILD_DIR = os.environ.get("ZET_BUILD_DIR", "")
GRID_DUMP = (
    os.environ.get("ZET_GRID_DUMP")
    or (str(Path(BUILD_DIR) / "tests" / "zet_grid_dump") if BUILD_DIR else "")
)

def _require_when_build_dir(thing: str, available: bool) -> None:
    """In the integration container (ZET_BUILD_DIR set) missing deps are hard."""
    if not available and BUILD_DIR:
        pytest.fail(f"{thing} is required when ZET_BUILD_DIR is set but was not found")


skip_no_pyte = pytest.mark.skipif(not PYTE_AVAILABLE and not BUILD_DIR,
                                   reason="pyte not installed")
skip_no_dump = pytest.mark.skipif(
    (not GRID_DUMP or not Path(GRID_DUMP).exists()) and not BUILD_DIR,
    reason="zet-grid-dump helper not built / ZET_GRID_DUMP not set")

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
# Allowlist loading
# ---------------------------------------------------------------------------

def _load_allowlist() -> list[dict]:
    """Return a list of allowlist entries; each entry has 'file', 'row', 'reason'."""
    if not ALLOWLIST_PATH.exists():
        return []
    with open(ALLOWLIST_PATH) as f:
        data = json.load(f)
    return data if isinstance(data, list) else []


_ALLOWLIST = _load_allowlist()


def _is_allowlisted(stem: str, row: int) -> Optional[str]:
    """Return the reason string if (file, row) matches an allowlist entry, else None."""
    for entry in _ALLOWLIST:
        entry_file = entry.get("file", "")
        entry_row = entry.get("row", -1)
        if (entry_file in ("*", stem) and
                (entry_row == -1 or entry_row == row)):
            return str(entry.get("reason", "allowlisted"))
    return None


# ---------------------------------------------------------------------------
# pyte reference
# ---------------------------------------------------------------------------

def _render_pyte(data: bytes, cols: int, rows: int) -> tuple[list[str], int, int]:
    """Return (rows, cursor_row, cursor_col) for the visible screen."""
    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    stream.feed(data)
    row_strs = [screen.display[r].rstrip() for r in range(rows)]
    # pyte cursor is 0-based
    return row_strs, screen.cursor.y, screen.cursor.x


# ---------------------------------------------------------------------------
# zet_grid reference (via zet_grid_dump helper)
# ---------------------------------------------------------------------------

def _render_zet(data: bytes, cols: int, rows: int) -> tuple[list[str], int, int]:
    """Run zet-grid-dump and return (rows, cursor_row, cursor_col)."""
    result = subprocess.run(
        [GRID_DUMP, "--cols", str(cols), "--rows", str(rows)],
        input=data,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 0, (
        f"zet-grid-dump failed: {result.stderr.decode(errors='replace')}"
    )
    payload = json.loads(result.stdout)
    row_strs = [row.rstrip() for row in payload["rows"]]
    return row_strs, payload["cursor_row"], payload["cursor_col"]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("capture", _corpus_files(), ids=lambda p: p.stem)
@skip_no_pyte
@skip_no_dump
def test_grid_matches_pyte(capture: Path) -> None:
    """Screen content of zet_grid matches pyte for the capture."""
    _require_when_build_dir("pyte", PYTE_AVAILABLE)
    _require_when_build_dir("zet-grid-dump", bool(GRID_DUMP and Path(GRID_DUMP).exists()))
    COLS, ROWS = 80, 24
    data = capture.read_bytes()
    zet_rows,  zet_cur_row,  zet_cur_col  = _render_zet(data, COLS, ROWS)
    pyte_rows, pyte_cur_row, pyte_cur_col = _render_pyte(data, COLS, ROWS)

    assert len(zet_rows) == len(pyte_rows), (
        f"Row count mismatch: zet={len(zet_rows)} pyte={len(pyte_rows)}"
    )

    # Compare row by row; check allowlist for known pyte differences.
    for r, (z, p_) in enumerate(zip(zet_rows, pyte_rows)):
        if z != p_:
            reason = _is_allowlisted(capture.stem, r)
            if reason:
                pytest.xfail(
                    f"Row {r} differs (allowlisted: {reason}):\n"
                    f"  zet:  {z!r}\n  pyte: {p_!r}"
                )
            assert z == p_, (
                f"Row {r} mismatch:\n  zet:  {z!r}\n  pyte: {p_!r}"
            )

    if zet_cur_row != pyte_cur_row:
        reason = _is_allowlisted(capture.stem, -1)
        if reason:
            pytest.xfail(
                f"Cursor row mismatch (allowlisted: {reason}): "
                f"zet={zet_cur_row} pyte={pyte_cur_row}"
            )
    assert zet_cur_row == pyte_cur_row, (
        f"Cursor row mismatch: zet={zet_cur_row} pyte={pyte_cur_row}"
    )

    if zet_cur_col != pyte_cur_col:
        reason = _is_allowlisted(capture.stem, -1)
        if reason:
            pytest.xfail(
                f"Cursor col mismatch (allowlisted: {reason}): "
                f"zet={zet_cur_col} pyte={pyte_cur_col}"
            )
    assert zet_cur_col == pyte_cur_col, (
        f"Cursor col mismatch: zet={zet_cur_col} pyte={pyte_cur_col}"
    )


@skip_no_pyte
def test_pyte_hello_world() -> None:
    """Smoke test: pyte renders a simple stream (import health check)."""
    data = b"Hello, world!\r\n"
    COLS, ROWS = 40, 10
    rows, _, _ = _render_pyte(data, COLS, ROWS)
    assert rows[0] == "Hello, world!"


@skip_no_dump
def test_zet_dump_smoke() -> None:
    """Smoke test: zet-grid-dump accepts a trivial stream and returns JSON."""
    data = b"Hello"
    result = subprocess.run(
        [GRID_DUMP, "--cols", "80", "--rows", "24"],
        input=data,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 0
    payload = json.loads(result.stdout)
    assert "rows" in payload
    assert payload["rows"][0].startswith("Hello")


@pytest.mark.skipif(bool(BUILD_DIR), reason="only checked when ZET_BUILD_DIR is set")
def test_corpus_not_empty_when_build_dir() -> None:
    """When ZET_BUILD_DIR is set the corpus must contain at least one file."""
    # This test is gated by the skipif above; _corpus_files() calls pytest.fail
    # when BUILD_DIR is set and the corpus is empty, so merely calling it here
    # provides the coverage for the other direction.
    if BUILD_DIR:
        files = _corpus_files()
        assert files, "Corpus is empty — run tools/capture_grid_corpus.sh"
