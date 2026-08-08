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
    "doctest:tests, header-only, never lands in the shipped binaries"
    "sodium:the one runtime dependency (from M1)"
    "PkgConfig:how libsodium is located, not a dependency of its own"
    "Threads:system"
)

mapfile -t cmake_files < <(find . -name 'CMakeLists.txt' -o -name '*.cmake' \
    | grep -v '/build/' | sort)

found=()
while read -r line; do
    [ -n "$line" ] || continue
    found+=("$line")
# pkg_check_modules is on the list because that is how libsodium actually
# arrives: it named no find_package, so the gate saw nothing and would have let
# the next dependency in the same way through unnoticed.
done < <(grep -hoE '(find_package|FetchContent_Declare|pkg_check_modules)\s*\(\s*[A-Za-z0-9_]+' "${cmake_files[@]}" 2>/dev/null \
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
        printf '  FAIL %s — not on the allowed list\n' "$dep"
        violations=$((violations + 1))
    fi
done

printf 'CMake files: %d, dependencies: %d, undeclared: %d\n' \
    "${#cmake_files[@]}" "${#found[@]}" "$violations"

if [ "$violations" -ne 0 ]; then
    echo
    echo "A new dependency is only added after a discussion:"
    echo "what it buys, what it costs in bytes and build seconds, what it"
    echo "would take to write instead. Then: this list, and an ADR in docs/design.md."
    exit 1
fi

echo "no undeclared dependencies"
