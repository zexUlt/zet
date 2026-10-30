#!/usr/bin/env bash
# Which CI jobs a change needs: the changed paths on stdin, one per line;
# `job=true|false` lines on stdout, one per job, in a fixed order.
#
# One place for the mapping, so that the three workflows agree on it, and a
# script rather than YAML, so that tools/ci_changes_test.sh can hold it to a
# table of cases. Every job stays in its workflow and every job reports: one
# that is not needed skips, and a skipped job counts as passed for a required
# check, which a workflow filtered out by path never does.
#
# The rule of thumb is a job runs when anything it builds or reads changed:
#   src/, tests/, fuzz/      the code each job builds, by directory below
#   the build                CMake files, presets, the Alpine package lists
#                            and the container scripts: every job
#   .github/ and this file   every job — a change to how CI decides is
#                            proven by running all of it
# Documentation (docs/, .claude/, *.md) needs nothing.
#
# Usage: git diff --name-only BASE...HEAD | tools/ci_changes.sh
set -uo pipefail

readonly JOBS=(test integration coverage checks macos fuzz macos_e2e systemd)

declare -A needed=()
for job in "${JOBS[@]}"; do
    needed[$job]=false
done

need() {
    local job
    for job in "$@"; do
        needed[$job]=true
    done
}

while IFS= read -r path; do
    [ -n "$path" ] || continue
    case "$path" in
        # Documentation first: a Markdown file anywhere is never code.
        docs/* | .claude/* | *.md) ;;

        .github/* | tools/ci_changes.sh | tools/ci_changes_test.sh)
            need "${JOBS[@]}" ;;

        CMakeLists.txt | */CMakeLists.txt | CMakePresets.json | cmake/* | \
        tools/alpine-packages.txt | tools/in_alpine.sh | tools/musl_build.sh)
            need "${JOBS[@]}" ;;

        # The protocol core: everything links it, coverage measures it, the
        # fuzzers drive it, the macOS client is built on it.
        src/core/* | src/crypto/* | src/wire/* | src/session/*)
            need test integration coverage checks macos fuzz macos_e2e systemd ;;

        # The VT parser and terminal grid model: compiled everywhere.
        src/grid/*)
            need test coverage checks macos fuzz ;;

        tests/grid/*)
            need test coverage checks macos ;;

        # zet_io and the layer above it: what the client and the agent are
        # made of. Not the fuzzers', not the coverage gate's.
        src/io/* | src/app/* | src/client/*)
            need test integration checks macos macos_e2e systemd ;;

        # The agent is Linux-only, but the macOS client talks to it end to end.
        src/agent/*)
            need test integration checks macos_e2e systemd ;;

        # zet-muxd: Linux-only, no macOS client path through it yet.
        src/mux/* | units/*)
            need test integration checks systemd ;;

        tests/core/* | tests/crypto/* | tests/wire/* | tests/session/*)
            need test coverage checks macos ;;

        tests/io/* | tests/client/* | tests/compile_fail/* | tests/main.cpp)
            need test checks macos ;;

        # The macOS end-to-end suite stands on the Linux harness's host.
        tests/integration/sshhost.py | tests/integration/harness.py | \
        tests/integration/e2e.py)
            need integration macos_e2e systemd ;;

        tests/integration/host.py)
            need integration macos_e2e ;;

        tests/systemd/*)
            need systemd ;;

        tests/integration/*)
            need integration ;;

        tests/macos_e2e/* | tools/macos_e2e_host.sh)
            need macos_e2e ;;

        tools/integration.sh | tools/integration-packages.txt)
            need integration macos_e2e ;;

        fuzz/* | tools/fuzz_run.sh)
            need fuzz checks ;;

        tools/check_coverage.sh)
            need coverage ;;

        tools/check_*.sh | tools/git-hooks/*)
            need checks ;;

        # Anything not listed above runs everything: a path nobody thought
        # about is not a path that is safe to skip.
        *)
            need "${JOBS[@]}" ;;
    esac
done

for job in "${JOBS[@]}"; do
    printf '%s=%s\n' "$job" "${needed[$job]}"
done
