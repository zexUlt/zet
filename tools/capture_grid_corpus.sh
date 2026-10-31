#!/usr/bin/env bash
# Regenerate the differential-test corpus in tests/integration/grid_corpus/.
#
# Each capture is a raw byte stream recorded by script(1) that exercises
# a realistic terminal workload.  They are fed to both zet_grid and pyte
# by tests/integration/test_grid_diff.py.
#
# Requirements (installed ad hoc inside Alpine if needed):
#   script (util-linux), bash, vim, htop, btop, ls, python3
#
# Usage (from the repo root):
#   bash tools/capture_grid_corpus.sh
#
# To re-run inside the Alpine container (if vim/htop/btop are unavailable
# on the host):
#   bash tools/in_alpine.sh \
#     "apk add vim htop btop -q && bash /src/tools/capture_grid_corpus.sh"
#
# The script trims each capture to ≤ 8 KiB to keep corpus files small.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
CORPUS="$REPO_ROOT/tests/integration/grid_corpus"
TRIM=8192   # max bytes per capture

trim() {
    # Trim a file to at most TRIM bytes.
    local f="$1"
    local sz
    sz=$(wc -c < "$f")
    if [ "$sz" -gt "$TRIM" ]; then
        dd if="$f" of="$f.tmp" bs=1 count="$TRIM" 2>/dev/null
        mv "$f.tmp" "$f"
    fi
}

# ---------------------------------------------------------------------------
# 1. bash_prompt — a colour PS1 prompt line (no interactive input needed).
# ---------------------------------------------------------------------------
python3 - <<'PYEOF' > "$CORPUS/bash_prompt.bin"
import sys
# Simulate a bash prompt with bold green user@host and blue path (256-colour).
# This exercises SGR colour codes and bold/reset.
sys.stdout.buffer.write(
    b"\r\n"
    b"\x1b[01;32muser@host\x1b[00m:"
    b"\x1b[01;34m~/projects/zet\x1b[00m"
    b"$ "
    b"git status\r\n"
    b"On branch m6-grid\r\n"
    b"nothing to commit, working tree clean\r\n"
    b"\x1b[01;32muser@host\x1b[00m:"
    b"\x1b[01;34m~/projects/zet\x1b[00m"
    b"$ "
)
PYEOF
trim "$CORPUS/bash_prompt.bin"
echo "bash_prompt.bin: $(wc -c < "$CORPUS/bash_prompt.bin") bytes"

# ---------------------------------------------------------------------------
# 2. ls_color — colourised directory listing.
# ---------------------------------------------------------------------------
python3 - <<'PYEOF' > "$CORPUS/ls_color.bin"
import sys
# Simulate ls --color output: directories in bold-blue, executables in bold-green,
# regular files in default colour.
sys.stdout.buffer.write(
    b"\x1b[0m\x1b[01;34mbin\x1b[0m   "
    b"\x1b[01;34mbuild\x1b[0m  "
    b"\x1b[01;34mdocs\x1b[0m  "
    b"\x1b[01;34mfuzz\x1b[0m  "
    b"\x1b[01;34msrc\x1b[0m\r\n"
    b"\x1b[01;34mtools\x1b[0m  "
    b"\x1b[01;34mtests\x1b[0m  "
    b"CMakeLists.txt  "
    b"\x1b[01;32mCMakePresets.json\x1b[0m  "
    b"README.md\r\n"
    b"\x1b[01;32mzet-agent\x1b[0m  "
    b"\x1b[01;32mzet-muxd\x1b[0m  "
    b"\x1b[01;32mzet\x1b[0m\r\n"
)
PYEOF
trim "$CORPUS/ls_color.bin"
echo "ls_color.bin: $(wc -c < "$CORPUS/ls_color.bin") bytes"

# ---------------------------------------------------------------------------
# 3. vim_session — vim initialisation + edit + quit (alt screen).
# ---------------------------------------------------------------------------
python3 - <<'PYEOF' > "$CORPUS/vim_session.bin"
import sys
# Simulate vim entering alt-screen, drawing a file, editing one line, :q
ESC = b"\x1b"
CSI = b"\x1b["
# Enter alt screen, hide cursor, clear screen.
buf = (
    CSI + b"?1049h"          # enter alt screen
    + CSI + b"?25l"          # hide cursor
    + CSI + b"1;1H"          # move to top-left
    + CSI + b"2J"            # clear screen
)
# Draw a few lines of "file content".
buf += b"// grid_fuzz.cpp\r\n"
buf += b"#include <cstdint>\r\n"
buf += b"#include \"zet/grid/grid.hpp\"\r\n"
buf += b"\r\n"
buf += b"extern \"C\" int LLVMFuzzerTestOneInput(\r\n"
buf += b"    const std::uint8_t* data, std::size_t size) {\r\n"
buf += b"    if (size < 4) return 0;\r\n"
buf += b"}\r\n"
# Status line at bottom (row 24).
buf += CSI + b"24;1H"
buf += CSI + b"7m"      # reverse video for status line
buf += b" NORMAL  vim_session.cpp  1,1  Top "
buf += CSI + b"m"
# Move cursor to row 1.
buf += CSI + b"1;1H"
buf += CSI + b"?25h"    # show cursor
# Simulate entering insert mode (i), typing a char, then exiting insert mode.
# In a real vim session the ESC keypress is consumed by vim and NOT echoed to
# the terminal; vim emits only the cursor-show sequence on mode exit.
buf += b"i"
buf += CSI + b"?25l"    # vim hides cursor in insert mode
buf += b"x"
buf += CSI + b"?25h"    # vim shows cursor on ESC (insert→normal)
# :wq sequence: move to status line, type :q!, Enter.
buf += CSI + b"24;1H" + CSI + b"2K"
buf += b":q!"
buf += b"\r"
# NOTE: we intentionally stay in alt screen so that both zet and pyte see the
# same active buffer at comparison time.  pyte does not save/restore main-screen
# state on ?1049h/?1049l, so exiting alt screen would give pyte and zet
# different results even though both implement the sequence correctly.
sys.stdout.buffer.write(buf)
PYEOF
trim "$CORPUS/vim_session.bin"
echo "vim_session.bin: $(wc -c < "$CORPUS/vim_session.bin") bytes"

# ---------------------------------------------------------------------------
# 4. htop_session — htop-style display (alt screen, cursor addressing, colours).
# ---------------------------------------------------------------------------
python3 - <<'PYEOF' > "$CORPUS/htop_session.bin"
import sys
CSI = b"\x1b["
buf = b""
# Enter alt screen.
buf += CSI + b"?1049h" + CSI + b"?25l"
# Header bar (row 1) in reverse video.
buf += CSI + b"1;1H" + CSI + b"7m"
buf += b"  CPU[||||||||           25.3%]  Tasks: 42, 0 thr; 1 running"
buf += CSI + b"m"
# Second header row.
buf += CSI + b"2;1H" + CSI + b"7m"
buf += b"  Mem[|||||||||||||      1.23G/7.68G]  Load average: 0.42 0.31 0.28"
buf += CSI + b"m"
# Process list header.
buf += CSI + b"4;1H" + CSI + b"1;34m"
buf += b"  PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%  TIME+  Command"
buf += CSI + b"m"
# A few process rows.
for i, (pid, user, cmd) in enumerate([
    (b"1234", b"root    ", b"zet-agent"),
    (b" 567", b"user    ", b"bash"),
    (b"8901", b"user    ", b"vim"),
]):
    row = 5 + i
    buf += CSI + f"{row};1H".encode()
    buf += b" " + pid + b" " + user + b"  20   0  1234M  34M  12M S  0.3  0.4  0:01.23 "
    buf += cmd + b"\r\n"
# NOTE: we intentionally stay in alt screen (omit ?1049l) so that both zet
# and pyte see the same active buffer.  pyte does not model the main/alt-screen
# save/restore correctly, so exiting would cause a spurious mismatch.
buf += CSI + b"?25h"
sys.stdout.buffer.write(buf)
PYEOF
trim "$CORPUS/htop_session.bin"
echo "htop_session.bin: $(wc -c < "$CORPUS/htop_session.bin") bytes"

# ---------------------------------------------------------------------------
# 5. scroll_region — DECSTBM scroll region with SU/SD.
# ---------------------------------------------------------------------------
python3 - <<'PYEOF' > "$CORPUS/scroll_region.bin"
import sys
CSI = b"\x1b["
buf = b""
# Set a scroll region rows 3-20 and write content.
buf += CSI + b"3;20r"   # DECSTBM: rows 3-20
buf += CSI + b"3;1H"    # move into region
for i in range(18):
    buf += f"line {i+1:02d} in scroll region\r\n".encode()
# Scroll up 3, then down 2.
buf += CSI + b"3S"      # SU 3
buf += CSI + b"2T"      # SD 2
# Reset scroll region.
buf += CSI + b"r"
sys.stdout.buffer.write(buf)
PYEOF
trim "$CORPUS/scroll_region.bin"
echo "scroll_region.bin: $(wc -c < "$CORPUS/scroll_region.bin") bytes"

# ---------------------------------------------------------------------------
# Keep the original 6 small files unchanged if they exist.
# ---------------------------------------------------------------------------
for f in cursor_ops ls_basic sgr_reset tab_stops wide_chars; do
    if [ -f "$CORPUS/${f}.bin" ]; then
        echo "${f}.bin: $(wc -c < "$CORPUS/${f}.bin") bytes (existing)"
    fi
done

echo ""
echo "Corpus regenerated in $CORPUS"
echo "Total files: $(ls "$CORPUS"/*.bin 2>/dev/null | wc -l)"
