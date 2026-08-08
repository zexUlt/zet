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

# docs/design.md §16 — «zet_wire и zet_session не ниже 90% строк, без единого
# исключения».
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
[ -x "$tests" ] || { echo "нет $tests — сборка тестов не дала бинаря" >&2; exit 2; }

# %p keeps the raw profiles apart: ctest runs every doctest case as its own
# process, and a single file would be overwritten by whoever exits last.
profraw=$root/$build/profraw
rm -rf "$profraw"
mkdir -p "$profraw"
ctest_log=$root/$build/ctest.log
if ! LLVM_PROFILE_FILE="$profraw/%p.profraw" ctest --preset "$preset" >"$ctest_log" 2>&1; then
    cat "$ctest_log" >&2
    echo "тесты упали — покрытие считать не по чему" >&2
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

for entry in "${GATED_TARGETS[@]}"; do
    target=${entry%%:*}
    dir=${entry##*:}

    if [ ! -d "$dir" ]; then
        printf '  skip %-12s %s ещё нет\n' "$target" "$dir"
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
        printf '  FAIL %-12s не попал в тестовый бинарь — измерять нечего\n' "$target"
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
        printf '    %-44s %4d/%-4d %6.2f%%\n' "${file#"$root"/}" \
            "$covered" "$total" \
            "$(awk -v c="$covered" -v t="$total" 'BEGIN { print c * 100 / t }')"
    done

    checked=$((checked + 1))
    percent=$(awk -v c="$target_covered" -v t="$target_total" 'BEGIN { printf "%.2f", c * 100 / t }')

    # Scaled integers: the threshold is whole percent, and 89.99% must not
    # round its way past it.
    if [ $((target_covered * 10000 / target_total)) -lt $((LINE_COVERAGE_THRESHOLD_PERCENT * 100)) ]; then
        printf '  FAIL %-12s %d/%d строк, %s%% < %d%%\n' \
            "$target" "$target_covered" "$target_total" "$percent" "$LINE_COVERAGE_THRESHOLD_PERCENT"
        violations=$((violations + 1))
    else
        printf '  ok   %-12s %d/%d строк, %s%% при пороге %d%%\n' \
            "$target" "$target_covered" "$target_total" "$percent" "$LINE_COVERAGE_THRESHOLD_PERCENT"
    fi
done

printf 'целей проверено: %d, недоборов: %d\n' "$checked" "$violations"

if [ "$checked" -eq 0 ]; then
    echo "нечего измерять: гейт оживёт вместе с целями из docs/design.md §16"
    exit 0
fi

if [ "$violations" -ne 0 ]; then
    echo
    echo "Покрытие ниже порога. Исключать файлы из измерения нельзя —"
    echo "именно так у ET из отчёта выпал разбор недоверенного ввода."
    echo "Дописывай тесты; см. docs/design.md §16."
    exit 1
fi

echo "покрытие ядра протокола выше порога"
