#!/usr/bin/env bash
# Invariant 1: the protocol core must not be able to terminate the process.
#
# Checked by linkage rather than by review, because review is exactly what
# fails here. EternalTerminal carries 164 LOG(FATAL) sites, several reachable
# from the network — a failed decrypt aborts the root daemon along with every
# other user's session.
#
# Usage: tools/check_no_abort.sh [build-dir]
set -uo pipefail

build=${1:-build/dev}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

if ! command -v nm >/dev/null 2>&1; then
    echo "no nm, gate skipped" >&2
    exit 2
fi

# Objects belonging to the protocol core. Main translation units are exempt:
# they are allowed to fail loudly before serving starts.
mapfile -t objects < <(find "$build" \
    -path "*zet_core.dir*" -name '*.o' \
    -o -path "*zet_wire.dir*" -name '*.o' \
    -o -path "*zet_session.dir*" -name '*.o' \
    -o -path "*zet_crypto.dir*" -name '*.o' 2>/dev/null | sort)

if [ ${#objects[@]} -eq 0 ]; then
    echo "no core object files in $build — build first" >&2
    exit 2
fi

banned='^(abort|exit|_exit|_Exit|quick_exit|__assert_fail|__assert_perror_fail|_ZSt9terminatev|__cxa_call_terminate)$'

violations=0
for obj in "${objects[@]}"; do
    # U = undefined, i.e. this object calls it and expects the linker to supply
    # it. That is precisely the thing we are forbidding.
    while read -r sym; do
        [ -n "$sym" ] || continue
        printf '  %s -> %s\n' "${obj#"$build"/}" "$sym"
        violations=$((violations + 1))
    done < <(nm --undefined-only --format=posix "$obj" 2>/dev/null |
             awk '{print $1}' | sed 's/@.*//' | grep -E "$banned")
done

printf 'core objects: %d, banned symbols: %d\n' "${#objects[@]}" "$violations"

if [ "$violations" -ne 0 ]; then
    echo
    echo "The protocol core may not end the process over data from the network."
    echo "Return the error via std::expected; see .claude/rules/invariants.md."
    exit 1
fi

echo "the core cannot end the process"
