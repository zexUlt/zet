#!/usr/bin/env bash
# Invariant 8: exactly one runtime dependency, and no dependency arrives by
# accident. Every find_package and FetchContent_Declare in the tree must be on
# the list below, which changes only through a deliberate review.
#
# Usage: tools/check_deps.sh
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

# name:why
allowed=(
    "doctest:тесты, header-only, в поставляемые бинари не попадает"
    "sodium:единственная рантайм-зависимость (с M1)"
    "PkgConfig:механизм поиска libsodium, не зависимость"
    "Threads:системная"
)

mapfile -t cmake_files < <(find . -name 'CMakeLists.txt' -o -name '*.cmake' \
    | grep -v '/build/' | sort)

found=()
while read -r line; do
    [ -n "$line" ] || continue
    found+=("$line")
done < <(grep -hoE '(find_package|FetchContent_Declare)\s*\(\s*[A-Za-z0-9_]+' "${cmake_files[@]}" 2>/dev/null \
         | sed -E 's/.*\(\s*//' | sort -u)

violations=0
for dep in "${found[@]}"; do
    ok=0
    for entry in "${allowed[@]}"; do
        [ "${entry%%:*}" = "$dep" ] && ok=1 && break
    done
    if [ "$ok" -eq 1 ]; then
        printf '  ok   %s\n' "$dep"
    else
        printf '  НЕТ  %s — не в списке разрешённых\n' "$dep"
        violations=$((violations + 1))
    fi
done

printf 'файлов CMake: %d, зависимостей: %d, незаявленных: %d\n' \
    "${#cmake_files[@]}" "${#found[@]}" "$violations"

if [ "$violations" -ne 0 ]; then
    echo
    echo "Новая зависимость добавляется только после обсуждения:"
    echo "что даёт, что весит в байтах и секундах сборки, чем заменяется"
    echo "своим кодом. Затем — в список в этом скрипте и ADR в docs/design.md."
    exit 1
fi

echo "незаявленных зависимостей нет"
