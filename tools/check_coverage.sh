#!/usr/bin/env bash
# docs/design.md §16: line coverage of the protocol core, with no way to opt a
# file out. ET excludes files from its coverage config by name, and the excluded
# one parses untrusted input — the only file whose number was worth having.
#
# Instrumentation rather than gcov, because the build is clang against musl and
# llvm-profdata/llvm-cov read that format directly.
#
# Usage: tools/check_coverage.sh [preset]
set -euo pipefail

# docs/design.md §16: zet_wire and zet_session at 90% of lines or better, with
# no exception for any file.
readonly LINE_COVERAGE_THRESHOLD_PERCENT=90

# Gated targets, name:source-dir. A target that does not exist yet is skipped:
# zet_session arrives on M2. zet_core is not on the list — §16 gates the two
# sans-I/O protocol libraries.
readonly GATED_TARGETS=(
    "zet_wire:src/wire"
    "zet_session:src/session"
)

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

preset=${1:-musl-coverage}
build=build/$preset

# The report has to be produced by the llvm-profdata that matches the clang
# which wrote the counters, and that pair lives in the container. Re-enter it
# once, then run the whole measurement inside.
if [ -z "${ZET_IN_ALPINE:-}" ]; then
    exec "$root/tools/in_alpine.sh" "ZET_IN_ALPINE=1 tools/check_coverage.sh $preset"
fi

cmake --preset "$preset" >/dev/null
cmake --build --preset "$preset" >/dev/null

tests=$build/tests/zet_tests
[ -x "$tests" ] || { echo "no $tests — the test build produced no binary" >&2; exit 2; }

# %p keeps the raw profiles apart: ctest runs every doctest case as its own
# process, and a single file would be overwritten by whoever exits last.
profraw=$root/$build/profraw
rm -rf "$profraw"
mkdir -p "$profraw"
ctest_log=$root/$build/ctest.log
if ! LLVM_PROFILE_FILE="$profraw/%p.profraw" ctest --preset "$preset" >"$ctest_log" 2>&1; then
    cat "$ctest_log" >&2
    echo "tests failed — there is nothing to measure coverage over" >&2
    exit 2
fi

profdata=$root/$build/coverage.profdata
llvm-profdata merge -sparse "$profraw"/*.profraw -o "$profdata"

# Everything the tests link, filtered per target below by path. Filtering with
# llvm-cov's own positional argument is not an option: given a path that matches
# no source file it reports the whole binary instead of nothing, so a target
# that fell out of the tests would come back looking covered.
lcov=$root/$build/coverage.lcov
llvm-cov export -format=lcov "$tests" -instr-profile="$profdata" >"$lcov"

checked=0
violations=0

# A machine-readable copy for the pull request comment. It lives next to the
# build rather than at a path handed in from outside: this script re-enters the
# container, where a host path such as RUNNER_TEMP does not exist.
markdown=$root/$build/coverage.md
markdown_rows=()

for entry in "${GATED_TARGETS[@]}"; do
    target=${entry%%:*}
    dir=${entry##*:}

    if [ ! -d "$dir" ]; then
        printf '  skip %-12s %s does not exist yet\n' "$target" "$dir"
        continue
    fi

    # lcov rather than the human-readable report: DA:<line>,<hits> is one fact
    # per line and needs no column alignment to read back.
    mapfile -t rows < <(awk -F'[:,]' -v prefix="$root/$dir/" '
            /^SF:/ { file = substr($0, 4); keep = (index(file, prefix) == 1); next }
            keep && /^DA:/ { total[file]++; if ($3 > 0) covered[file]++ }
            END { for (f in total) printf "%s %d %d\n", f, covered[f], total[f] }
        ' "$lcov" | sort)

    # The directory exists, so the target does; no instrumented lines means it
    # never got linked into the tests. That is an exclusion by accident, and
    # silently passing it is the failure mode this gate is about.
    if [ ${#rows[@]} -eq 0 ]; then
        printf '  FAIL %-12s not linked into the test binary — nothing to measure\n' "$target"
        checked=$((checked + 1))
        violations=$((violations + 1))
        continue
    fi

    printf '%s\n' "$target"

    target_covered=0
    target_total=0
    for row in "${rows[@]}"; do
        read -r file covered total <<<"$row"
        target_covered=$((target_covered + covered))
        target_total=$((target_total + total))
        file_percent=$(awk -v c="$covered" -v t="$total" 'BEGIN { printf "%.2f", c * 100 / t }')
        printf '    %-44s %4d/%-4d %6s%%\n' "${file#"$root"/}" \
            "$covered" "$total" "$file_percent"
        markdown_rows+=("| \`${file#"$root"/}\` | $covered/$total | $file_percent% |")
    done

    checked=$((checked + 1))
    percent=$(awk -v c="$target_covered" -v t="$target_total" 'BEGIN { printf "%.2f", c * 100 / t }')

    # Scaled integers: the threshold is whole percent, and 89.99% must not
    # round its way past it.
    markdown_rows+=("| **$target** | **$target_covered/$target_total** | **$percent%** |")

    if [ $((target_covered * 10000 / target_total)) -lt $((LINE_COVERAGE_THRESHOLD_PERCENT * 100)) ]; then
        printf '  FAIL %-12s %d/%d lines, %s%% < %d%%\n' \
            "$target" "$target_covered" "$target_total" "$percent" "$LINE_COVERAGE_THRESHOLD_PERCENT"
        violations=$((violations + 1))
    else
        printf '  ok   %-12s %d/%d lines, %s%% against a %d%% threshold\n' \
            "$target" "$target_covered" "$target_total" "$percent" "$LINE_COVERAGE_THRESHOLD_PERCENT"
    fi
done

printf 'targets checked: %d, below threshold: %d\n' "$checked" "$violations"

{
        # The marker is how the workflow finds its own comment to update
        # instead of leaving a new one on every push.
        echo "<!-- zet-coverage -->"
        echo "### Line coverage of the protocol core"
        echo
        echo "| | lines | |"
        echo "|---|---|---|"
        printf '%s\n' "${markdown_rows[@]}"
        echo
        if [ "$violations" -ne 0 ]; then
            echo "Below the ${LINE_COVERAGE_THRESHOLD_PERCENT}% threshold from docs/design.md §16."
        else
            echo "Threshold ${LINE_COVERAGE_THRESHOLD_PERCENT}%, from docs/design.md §16."
        fi
} >"$markdown"

if [ "$checked" -eq 0 ]; then
    echo "nothing to measure: the gate wakes up with the targets from §16"
    exit 0
fi

if [ "$violations" -ne 0 ]; then
    echo
    echo "Coverage is below the threshold. Excluding a file from the measurement"
    echo "is not an option — that is how ET dropped its untrusted-input parser"
    echo "from the report. Write the tests; see docs/design.md §16."
    exit 1
fi

echo "the protocol core is above the coverage threshold"
