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

expect "fuzz/session_fuzz.cpp" "fuzz checks"
expect "tools/check_no_abort.sh" "checks"
expect "tools/check_coverage.sh" "coverage"

# A path the table does not know runs everything.
expect "something/new.txt" "$all"

# The union of what each path needs.
expect "tests/integration/test_mux.py,fuzz/mux_fuzz.cpp" "integration fuzz checks"

if [ "$failures" -ne 0 ]; then
    echo "$failures case(s) wrong"
    exit 1
fi
echo "ci_changes: every case as expected"
