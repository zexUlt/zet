#!/usr/bin/env bash
# gen_diff_corpus.sh — generate differential regression seeds for grid_fuzz.
#
# Each seed is a binary file consumed by LLVMFuzzerTestOneInput:
#   byte 0:  (cols0 / 2 - 1) & 0x7F   (initial grid columns)
#   byte 1:  (rows0 / 2 - 1) & 0x3F   (initial grid rows)
#   byte 2:  (cols1 / 2 - 1) & 0x7F   (resize-target columns)
#   byte 3:  (rows1 / 2 - 1) & 0x3F   (resize-target rows)
#   bytes 4…: VT sequence fed through Feed()
#
# Seeds target the fixes introduced in the m6-grid branch:
#   - DCH on wide-char continuation cell (orphan-blank fix)
#   - CUU/CUD scroll-margin clamping
#   - Tab stops after column-grow Resize
#   - CSI private-marker guards (DECSTBM with intermediate, CSI > 7 l)
#   - Combining mark at PendingWrap boundary

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$REPO_ROOT/fuzz/corpus/grid"
mkdir -p "$OUTDIR"

python3 - "$OUTDIR" <<'PYEOF'
import sys
import os

outdir = sys.argv[1]

def header(c0: int, r0: int, c1: int, r1: int) -> bytes:
    """Encode the 4-byte grid-dimensions header."""
    return bytes([
        (c0 // 2 - 1) & 0x7F,
        (r0 // 2 - 1) & 0x3F,
        (c1 // 2 - 1) & 0x7F,
        (r1 // 2 - 1) & 0x3F,
    ])

def write_seed(name: str, c0: int, r0: int, c1: int, r1: int, payload: bytes):
    path = os.path.join(outdir, name)
    with open(path, "wb") as f:
        f.write(header(c0, r0, c1, r1) + payload)

# 1. DCH on wide-char continuation cell must blank both halves.
#    Write 中 (U+4E2D, UTF-8: e4 b8 ad), move cursor left one col, DCH.
write_seed("dch_wide_continuation", 20, 10, 20, 10,
    b"\xe4\xb8\xad\x1b[1D\x1b[P")

# 2. Wide character placed at the last column (EOL edge case).
write_seed("wide_at_eol", 20, 10, 20, 10,
    b"\x1b[1;20H\xe4\xb8\xad")

# 3. CUU with cursor inside scroll region must stop at ScrollTop.
#    Scroll region rows 5–20 (1-based); cursor at row 11; CUU 100.
write_seed("cuu_stops_at_scroll_top", 80, 24, 80, 24,
    b"\x1b[5;20r\x1b[11;1H\x1b[100A")

# 4. CUD with cursor inside scroll region must stop at ScrollBot.
#    Scroll region rows 5–20 (1-based); cursor at row 15; CUD 100.
write_seed("cud_stops_at_scroll_bot", 80, 24, 80, 24,
    b"\x1b[5;20r\x1b[15;1H\x1b[100B")

# 5. Tab stop at col 16 must exist after resize grows from 10 to 80 columns.
#    Move cursor to col 9 (1-based: col 10), send HT.
write_seed("tab_after_col_grow", 10, 10, 80, 10,
    b"\x1b[1;10H\t")

# 6. DECSTBM with intermediate byte (CSI 5 ; 20 $ r) must not set scroll region.
write_seed("decstbm_with_dollar_intermediate", 80, 24, 80, 24,
    b"\x1b[5;20$r")

# 7. CSI > 7 l must not clear DECAWM (private marker '>' is not '?').
write_seed("csi_gt_decrst_no_awm", 80, 24, 80, 24,
    b"\x1b[?7h\x1b[>7l")

# 8. Combining mark at PendingWrap boundary: base codepoint kept, mark dropped.
#    Move to last column (col 10, 1-based), send base 'A' + combining acute.
write_seed("combining_at_pendingwrap", 10, 5, 10, 5,
    b"\x1b[1;10HA\xcc\x81")

# 9. Resize shrink clipping a wide char: continuation must not become orphan.
#    Place 中 at col 19 (0-based) of a 20-col grid, shrink to 18 cols.
write_seed("resize_shrink_clips_wide", 20, 10, 18, 10,
    b"\x1b[1;19H\xe4\xb8\xad")

# 10. Double resize (grow then shrink) with wide chars — exercises the path
#     in grid_fuzz that resizes twice per input.
write_seed("grow_shrink_double_resize", 40, 20, 20, 10,
    b"\xe4\xb8\xad" * 10)

print(f"Generated {len(os.listdir(outdir))} seeds in {outdir}")
PYEOF
