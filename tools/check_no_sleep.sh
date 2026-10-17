#!/usr/bin/env bash
# Invariant 3: nothing in zet waits by sleeping.
#
# Waiting goes through the event loop's timer queue: the epoll timeout is
# computed from the nearest deadline, and there are no fixed ticks. A sleep in
# a loop that carries sessions stops every one of them for its duration.
# EternalTerminal has 25 of these, including sleep(1) inside the loop that
# serves a session.
#
# A grep rather than nm, because the calls this is after are as often inline
# templates (std::this_thread::sleep_for) as library symbols. Tests are out of
# scope by the wording of the invariant; they wait through the poller anyway.
#
# Usage: tools/check_no_sleep.sh
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

# Where shipped code lives. fuzz/ is in: a fuzzer that sleeps measures nothing.
readonly SCANNED_DIRS=(src fuzz)

# A call, not a mention: the name followed by an opening parenthesis. A
# comment that quotes one, as this file's header does, is still a match, and
# that is deliberate — the core's comments have no reason to show how to
# sleep either.
banned='(^|[^A-Za-z0-9_])(sleep|usleep|nanosleep|clock_nanosleep|sleep_for|sleep_until)[[:space:]]*\('

files=$(find "${SCANNED_DIRS[@]}" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) 2>/dev/null | sort)
count=$(printf '%s\n' "$files" | grep -c .)

if [ "$count" -eq 0 ]; then
    echo "no sources under ${SCANNED_DIRS[*]}" >&2
    exit 2
fi

# shellcheck disable=SC2086 # the list is one path per line, without spaces
matches=$(grep -nE "$banned" $files)
violations=$(printf '%s' "$matches" | grep -c .)

[ -z "$matches" ] || printf '%s\n' "$matches" | sed 's/^/  /'

printf 'sources: %d, sleeping calls: %d\n' "$count" "$violations"

if [ "$violations" -ne 0 ]; then
    echo
    echo "Wait on the poller with a deadline instead; see invariant 3 in"
    echo ".claude/rules/invariants.md and docs/design.md §10."
    exit 1
fi

echo "nothing sleeps"
