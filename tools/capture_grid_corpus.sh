#!/usr/bin/env bash
# Regenerate the differential-test corpus in tests/integration/grid_corpus/.
#
# Real captures are recorded via Python's pty module inside an Alpine container
# at TERM=xterm-256color, 80x24.  Run from inside the container:
#
#   bash tools/in_alpine.sh \
#     "apk add -q util-linux vim htop && bash /src/tools/capture_grid_corpus.sh"
#
# Each real capture spawns the app in a fresh PTY (80x24), collects output for
# up to N seconds (or until the process exits), and writes raw bytes to the
# corpus file.  No script(1) header/footer is included.
#
# Synthetic files (*_synthetic.bin) test specific sequences that require exact
# control over the byte stream.  They are named *_synthetic.bin so their origin
# is unambiguous.  scroll_region_synthetic.bin exercises DECSTBM + SU/SD, which
# pyte does not implement; differences are listed in allowlist.json.
#
# Captures larger than 8 KiB are trimmed at the last newline at or before that
# boundary (not mid-escape).

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
CORPUS="$REPO_ROOT/tests/integration/grid_corpus"

# Trim a file to ≤ 8 KiB, cutting at the last \n at or before that boundary.
trim_at_newline() {
    local f="$1"
    python3 - "$f" <<'PYEOF'
import sys
path = sys.argv[1]
data = open(path, 'rb').read()
limit = 8192
if len(data) <= limit:
    sys.exit(0)
cut = data.rfind(b'\n', 0, limit + 1)
if cut < 0:
    cut = limit  # no newline; cut at byte limit
else:
    cut += 1     # include the newline itself
with open(path, 'wb') as fh:
    fh.write(data[:cut])
print(f"  trimmed to {cut} bytes at newline boundary", flush=True)
PYEOF
}

# Capture the terminal output of a command via Python's pty module.
# Waits up to DURATION seconds (default 3) for the process to exit.
# Sets TERM=xterm-256color, COLUMNS=80, LINES=24 and the PTY geometry.
capture_pty() {
    local out="$1"
    local duration="${2:-3}"
    shift 2
    # Remaining arguments are the command + its arguments.
    python3 - "$out" "$duration" "$@" <<'PYEOF'
import pty, os, sys, select, fcntl, termios, struct, time, signal

outfile  = sys.argv[1]
duration = float(sys.argv[2])
cmd      = sys.argv[3:]
rows, cols = 24, 80

master_fd, slave_fd = pty.openpty()
# Set geometry on the slave before fork.
ws = struct.pack("HHHH", rows, cols, 0, 0)
fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, ws)

pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
    for fd in range(3):
        os.dup2(slave_fd, fd)
    if slave_fd > 2:
        os.close(slave_fd)
    os.close(master_fd)
    os.environ["TERM"]    = "xterm-256color"
    os.environ["COLUMNS"] = str(cols)
    os.environ["LINES"]   = str(rows)
    os.execvp(cmd[0], cmd)
    os._exit(127)

os.close(slave_fd)
buf      = b""
deadline = time.time() + duration
while True:
    remaining = deadline - time.time()
    if remaining <= 0:
        break
    try:
        r, _, _ = select.select([master_fd], [], [], min(remaining, 0.1))
    except (InterruptedError, ValueError):
        break
    if r:
        try:
            chunk = os.read(master_fd, 4096)
            if not chunk:
                break
            buf += chunk
        except OSError:
            break
    # Check if child exited naturally.
    wpid, _ = os.waitpid(pid, os.WNOHANG)
    if wpid == pid:
        # Drain any final output.
        while True:
            try:
                r2, _, _ = select.select([master_fd], [], [], 0.05)
                if r2:
                    chunk = os.read(master_fd, 4096)
                    if chunk:
                        buf += chunk
                    else:
                        break
                else:
                    break
            except OSError:
                break
        break

try:
    os.close(master_fd)
except OSError:
    pass
try:
    os.kill(pid, signal.SIGTERM)
    os.waitpid(pid, 0)
except (ProcessLookupError, ChildProcessError):
    pass

with open(outfile, 'wb') as fh:
    fh.write(buf)
PYEOF
}

# Use PID-based paths (BusyBox mktemp does not accept full-path templates).
TMPBASE="/tmp/zet-corpus-$$"

# ---------------------------------------------------------------------------
# 1. bash_session — coloured PS1, ls --color=always on a sample directory.
#    Source: Python pty.fork → bash --norc -c 'ls --color=always ...'
# ---------------------------------------------------------------------------
TMPLS="${TMPBASE}-ls"
mkdir -p "$TMPLS/src" "$TMPLS/docs" "$TMPLS/tests"
touch "$TMPLS/CMakeLists.txt" "$TMPLS/README.md" "$TMPLS/LICENSE"
printf '#!/bin/sh\nexec zet "$@"\n' > "$TMPLS/run.sh"
chmod +x "$TMPLS/run.sh"

capture_pty "$CORPUS/bash_session.bin" 3 \
  bash --norc -c \
  "PS1='\[\033[01;32m\]user@alpine\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]\$ '; ls --color=always ${TMPLS}; echo; echo Done"
rm -rf "$TMPLS"
trim_at_newline "$CORPUS/bash_session.bin"
echo "bash_session.bin: $(wc -c < "$CORPUS/bash_session.bin") bytes  [pty: bash --norc ls --color=always]"

# ---------------------------------------------------------------------------
# 2. ls_color — ls --color=always on a populated directory.
#    Source: Python pty.fork → ls --color=always <dir>
# ---------------------------------------------------------------------------
TMPLS2="${TMPBASE}-ls2"
mkdir -p "$TMPLS2/bin" "$TMPLS2/src" "$TMPLS2/docs"
touch "$TMPLS2/CMakeLists.txt" "$TMPLS2/README.md"
printf '#!/bin/sh\n' > "$TMPLS2/build.sh"
chmod +x "$TMPLS2/build.sh"

capture_pty "$CORPUS/ls_color.bin" 3 \
  ls --color=always "$TMPLS2"
rm -rf "$TMPLS2"
trim_at_newline "$CORPUS/ls_color.bin"
echo "ls_color.bin: $(wc -c < "$CORPUS/ls_color.bin") bytes  [pty: ls --color=always]"

# ---------------------------------------------------------------------------
# 3. vim_session — open a file, scroll to bottom (exercises scroll region),
#    then quit.  Alt-screen enter (?1049h) and exit (?1049l) are included.
#    pyte does not save/restore main-screen state on ?1049l; rows that differ
#    after alt-screen exit are listed in allowlist.json.
#    Source: Python pty.fork → vim -c 'normal Gzt' -c 'wq!'
# ---------------------------------------------------------------------------
TMPVIM="${TMPBASE}-vim.txt"
seq 1 50 | sed 's/^/Line /' > "$TMPVIM"

# -c 'set lines=24 columns=80'  reinforce terminal geometry
# -c 'normal Gzt'               jump to last line and scroll so it is at top,
#                               exercising vim's scroll region (DECSTBM)
# -c 'wq!'                      write and quit — emits ?1049l (exit alt screen)
capture_pty "$CORPUS/vim_session.bin" 5 \
  vim -c 'set lines=24 columns=80' -c 'normal Gzt' -c 'wq!' "$TMPVIM"
rm -f "$TMPVIM"
trim_at_newline "$CORPUS/vim_session.bin"
echo "vim_session.bin: $(wc -c < "$CORPUS/vim_session.bin") bytes  [pty: vim -c 'normal Gzt' -c 'wq!']"

# ---------------------------------------------------------------------------
# 4. htop_session — real htop for up to 2 seconds, then natural exit.
#    Alt-screen enter and exit are included; see allowlist.json for pyte diffs.
#    Source: Python pty.fork → htop (killed after 2 s via SIGTERM)
# ---------------------------------------------------------------------------
capture_pty "$CORPUS/htop_session.bin" 2 htop
trim_at_newline "$CORPUS/htop_session.bin"
echo "htop_session.bin: $(wc -c < "$CORPUS/htop_session.bin") bytes  [pty: htop (2 s)]"

# ---------------------------------------------------------------------------
# 5. scroll_region_synthetic — DECSTBM + SU/SD.
#    Synthetic: pyte does not implement SU/SD within scroll regions.
#    Differences are listed in allowlist.json.
# ---------------------------------------------------------------------------
python3 - <<'PYEOF' > "$CORPUS/scroll_region_synthetic.bin"
import sys
# Synthetic: exercises DECSTBM scroll region with SU (CSI Ps S) and SD (CSI Ps T).
# pyte does not implement SU/SD within scroll regions; the resulting differences
# are documented in tests/integration/grid_corpus/allowlist.json.
CSI = b"\x1b["
buf = CSI + b"3;20r"   # DECSTBM: scroll region rows 3-20
buf += CSI + b"3;1H"   # move into region
for i in range(18):
    buf += f"line {i+1:02d} in scroll region\r\n".encode()
buf += CSI + b"3S"     # SU 3 — scroll up 3 lines within region
buf += CSI + b"2T"     # SD 2 — scroll down 2 lines within region
buf += CSI + b"r"      # reset scroll region
sys.stdout.buffer.write(buf)
PYEOF
echo "scroll_region_synthetic.bin: $(wc -c < "$CORPUS/scroll_region_synthetic.bin") bytes  [synthetic: DECSTBM+SU/SD]"

# ---------------------------------------------------------------------------
# Keep existing small synthetic files unchanged.
# ---------------------------------------------------------------------------
for f in cursor_ops ls_basic sgr_reset tab_stops wide_chars; do
    if [ -f "$CORPUS/${f}.bin" ]; then
        echo "${f}.bin: $(wc -c < "$CORPUS/${f}.bin") bytes  [existing synthetic]"
    fi
done

echo ""
echo "Corpus regenerated in $CORPUS"
echo "Total .bin files: $(ls "$CORPUS"/*.bin 2>/dev/null | wc -l)"
