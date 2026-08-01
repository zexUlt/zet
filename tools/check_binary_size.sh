#!/usr/bin/env bash
# Invariant 6: binary size is a number in CI, not an aspiration.
#
# Budgets come from docs/design.md §14. Exceeding one is not automatically a
# failure of the feature or of the budget — it is a decision to make out loud,
# with the byte count in hand.
#
# Usage: tools/check_binary_size.sh [build-dir]
set -uo pipefail

build=${1:-build/release}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

# binary:budget in KiB
budgets=(
    "zet:1229"
    "zet-agent:1536"
    "zet-muxd:614"
)

checked=0
violations=0

for entry in "${budgets[@]}"; do
    name=${entry%%:*}
    budget=${entry##*:}
    path=$(find "$build" -type f -name "$name" -perm -u+x 2>/dev/null | head -1)

    if [ -z "$path" ]; then
        printf '  skip %-10s ещё не собирается\n' "$name"
        continue
    fi

    size=$(( $(stat -c %s "$path") / 1024 ))
    checked=$((checked + 1))
    percent=$(( size * 100 / budget ))

    if [ "$size" -gt "$budget" ]; then
        printf '  FAIL %-10s %d KiB > %d KiB (%d%%)\n' "$name" "$size" "$budget" "$percent"
        violations=$((violations + 1))
    else
        printf '  ok   %-10s %d KiB / %d KiB (%d%%)\n' "$name" "$size" "$budget" "$percent"
    fi
done

printf 'бинарей проверено: %d, превышений: %d\n' "$checked" "$violations"

if [ "$checked" -eq 0 ]; then
    echo "нечего измерять: бинари появятся на M4"
    exit 0
fi

if [ "$violations" -ne 0 ]; then
    echo
    echo "Не раздувай бюджет молча и не выкидывай фичу молча."
    echo "Вынеси расхождение в docs/design.md §18 с цифрами."
    exit 1
fi

echo "все бинари в бюджете"
