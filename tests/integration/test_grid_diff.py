"""Differential test: zet_grid vs pyte (design.md §16).

Feeds captured terminal streams through both zet_grid (via the CLI tool
zet-grid-dump, built alongside zet_tests) and pyte, and asserts that the
visible screen content matches.

The test skips when:
  - pyte is not installed (importable),
  - the zet-grid-dump helper is not on PATH / ZET_GRID_DUMP,
  - we are not in the integration container (no ZET_BUILD_DIR env var).

The corpus lives in tests/integration/grid_corpus/ — small captures produced
by script(1) and trimmed to a few hundred bytes each.  The test parametrises
over every .bin file in that directory.
"""

from __future__ import annotations

import os
import subprocess
import json
from pathlib import Path

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

skip_no_pyte     = pytest.mark.skipif(not PYTE_AVAILABLE,
                                      reason="pyte not installed")
skip_no_dump     = pytest.mark.skipif(
    not GRID_DUMP or not Path(GRID_DUMP).exists(),
    reason="zet-grid-dump helper not built / ZET_GRID_DUMP not set")

# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------

CORPUS_DIR = Path(__file__).parent / "grid_corpus"


def _corpus_files() -> list[Path]:
    if not CORPUS_DIR.is_dir():
        return []
    return sorted(CORPUS_DIR.glob("*.bin"))


# ---------------------------------------------------------------------------
# pyte reference
# ---------------------------------------------------------------------------

def _render_pyte(data: bytes, cols: int, rows: int) -> list[str]:
    """Return the visible screen as a list of stripped row strings."""
    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    stream.feed(data)
    return [screen.display[r].rstrip() for r in range(rows)]


# ---------------------------------------------------------------------------
# zet_grid reference (via zet_grid_dump helper)
# ---------------------------------------------------------------------------

def _render_zet(data: bytes, cols: int, rows: int) -> list[str]:
    """Run zet-grid-dump and return the visible screen as a list of rows."""
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
    return [row.rstrip() for row in payload["rows"]]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("capture", _corpus_files(), ids=lambda p: p.stem)
@skip_no_pyte
@skip_no_dump
def test_grid_matches_pyte(capture: Path) -> None:
    """Screen content of zet_grid matches pyte for the capture."""
    COLS, ROWS = 80, 24
    data = capture.read_bytes()
    zet_rows  = _render_zet(data, COLS, ROWS)
    pyte_rows = _render_pyte(data, COLS, ROWS)

    # Compare row by row so pytest shows which line differs.
    for r, (z, p_) in enumerate(zip(zet_rows, pyte_rows)):
        assert z == p_, (
            f"Row {r} mismatch:\n  zet:  {z!r}\n  pyte: {p_!r}"
        )


@skip_no_pyte
def test_pyte_hello_world() -> None:
    """Smoke test: pyte renders a simple stream (import health check)."""
    data = b"Hello, world!\r\n"
    COLS, ROWS = 40, 10
    rows = _render_pyte(data, COLS, ROWS)
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
