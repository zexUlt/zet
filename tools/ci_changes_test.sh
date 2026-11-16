#!/usr/bin/env bash
# Holds tools/ci_changes.sh to a table: for each change, which jobs run.
# A wrong answer here is a pull request that merges untested, so the table
# names the jobs that must run and the ones that must not, both ways.
#
# Usage: tools/ci_changes_test.sh
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
classify="$root/tools/ci_changes.sh"

failures=0

# expect "<paths, comma-separated>" "<jobs that run, space-separated>"
expect() {
    local paths=$1 want=$2 got
    got=$(printf '%s\n' "${paths//,/$'\n'}" | "$classify" |
          awk -F= '$2 == "true" {print $1}' | sort | tr '\n' ' ')
    local -a jobs=()
    read -r -a jobs <<<"$want"
    want=""
    [ "${#jobs[@]}" -eq 0 ] || want=$(printf '%s\n' "${jobs[@]}" | sort | tr '\n' ' ')
    if [ "$got" != "$want" ]; then
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$paths" "$want" "$got"
        failures=$((failures + 1))
    fi
}

all="test integration coverage checks macos fuzz macos_e2e systemd"

expect "docs/design.md" ""
expect "README.md,.claude/rules/invariants.md,tests/integration/README.md" ""
expect "" ""

expect ".github/workflows/ci.yml" "$all"
expect "tools/ci_changes.sh" "$all"
expect "CMakeLists.txt" "$all"
expect "src/io/CMakeLists.txt" "$all"
expect "tools/alpine-packages.txt" "$all"

expect "src/wire/message.cpp" "$all"
expect "src/session/channel.cpp" "$all"
expect "src/io/poller_kqueue.cpp" "test integration checks macos macos_e2e systemd"
expect "src/client/client.cpp" "test integration checks macos macos_e2e systemd"
expect "src/agent/agent.cpp" "test integration checks macos_e2e systemd"
expect "src/mux/mux.cpp" "test integration checks systemd"
expect "units/zet-muxd.socket" "test integration checks systemd"

expect "tests/wire/message_test.cpp" "test coverage checks macos"
expect "tests/io/poller_test.cpp" "test checks macos"
expect "tests/integration/test_mux.py" "integration"
expect "tests/integration/sshhost.py" "integration macos_e2e systemd"
expect "tests/integration/host.py" "integration macos_e2e"
expect "tests/systemd/test_systemd.py" "systemd"
expect "tests/macos_e2e/test_client.py" "macos_e2e"
expect "tests/macos_e2e/test_deliver.py" "macos_e2e"

expect "fuzz/session_fuzz.cpp" "fuzz checks"
expect "tools/check_no_abort.sh" "checks"
expect "tools/check_coverage.sh" "coverage"

# M5.5 delivery paths.
expect "src/client/deliver.cpp" "test integration checks macos macos_e2e systemd"
expect "src/client/deliver.hpp" "test integration checks macos macos_e2e systemd"
expect "src/wire/probe.cpp" "$all"
expect "fuzz/probe_fuzz.cpp" "fuzz checks"
expect "tests/integration/test_deliver.py" "integration"
expect "tools/integration-packages.txt" "integration macos_e2e"

# M6 grid paths.
expect "src/grid/grid.cpp" "test integration coverage checks macos fuzz"
expect "src/grid/vt_parser.cpp" "test integration coverage checks macos fuzz"
expect "tests/grid/grid_test.cpp" "test integration coverage checks macos"
expect "tests/grid/grid_dump_main.cpp" "test integration coverage checks macos"
expect "tests/integration/test_grid_diff.py" "integration"
expect "src/predict/engine.cpp" "test coverage checks macos fuzz"
expect "tests/predict/predict_epoch_test.cpp" "test coverage checks macos"
expect "src/grid/snapshot.cpp" "test integration coverage checks macos fuzz"
expect "tests/grid/snapshot_test.cpp" "test integration coverage checks macos"
expect "fuzz/snapshot_fuzz.cpp" "fuzz checks"

# A path the table does not know runs everything.
expect "something/new.txt" "$all"

# The union of what each path needs.
expect "tests/integration/test_mux.py,fuzz/mux_fuzz.cpp" "integration fuzz checks"

# The BASE form on a real history. A git hook running this test exports
# these, and they would point every git call below at the hook's repository.
unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE
repo=$(mktemp -d)
trap 'rm -rf "$repo"' EXIT
git_() { git -C "$repo" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }

# expect_base <name> <base> <jobs that run>: the jobs that run, and nothing
# but job=true|false lines on stdout, which goes to $GITHUB_OUTPUT.
expect_base() {
    local name=$1 base=$2 want=$3 out got
    out=$(cd "$repo" && "$classify" "$base" 2>/dev/null)
    if printf '%s\n' "$out" | grep -qvE '^[a-z_0-9]+=(true|false)$'; then
        printf 'FAIL %s: stdout is not only job=value lines:\n%s\n' "$name" "$out"
        failures=$((failures + 1))
        return
    fi
    got=$(printf '%s\n' "$out" | awk -F= '$2 == "true" {print $1}' | sort | tr '\n' ' ')
    if [ "$got" != "$want" ]; then
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$name" "$want" "$got"
        failures=$((failures + 1))
    fi
}

git_ init -q -b trunk
mkdir -p "$repo/src/mux" "$repo/docs"
echo 'int main() {}' > "$repo/src/mux/mux.cpp"
echo 'notes' > "$repo/docs/notes.md"
git_ add -A && git_ commit -q -m base
base=$(git_ rev-parse HEAD)

# Code moved into documentation is still a change to that code.
git_ checkout -q -b move
git_ mv src/mux/mux.cpp docs/mux.md
git_ commit -q -m move
expect_base "rename src/mux/mux.cpp -> docs/mux.md" "$base" "checks integration systemd test "

# BASE...HEAD: what trunk did after the branch point is not this change.
git_ checkout -q trunk
echo 'int main() { return 0; }' > "$repo/src/mux/mux.cpp"
git_ commit -q -am trunk-moves-on
git_ checkout -q -b docs-only "$base"
echo 'more notes' >> "$repo/docs/notes.md"
git_ commit -q -am docs
expect_base "docs on a branch behind trunk" trunk ""

for bad in "" no-such-ref; do
    if (cd "$repo" && "$classify" "$bad" >/dev/null 2>&1); then
        echo "FAIL BASE '$bad' exits with 0"
        failures=$((failures + 1))
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "$failures case(s) wrong"
    exit 1
fi
echo "ci_changes: every case as expected"
