#!/usr/bin/env bash
# The client builds on Linux and macOS (design.md §10, §12), so the code on
# its path may only use what both have. A platform's own calls live in its
# backend files — poller_epoll.cpp, poller_kqueue.cpp — and which backend is
# built is said once, in poller_backend.hpp. This gate keeps the rest from
# growing Linux-isms (or macOS-isms) that would only show when the other
# platform is next built.
#
# Comments, // and /* */ alike, are stripped before matching: the Poller
# contract names epoll and kqueue on purpose, and a name inside a block
# comment neither counts against a file nor hides the code after it.
#
# Usage: tools/check_portable.sh
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

# The client's path: its loop, what it shares with the agent, and the part
# of zet_io both platforms build.
readonly PORTABLE=(
    src/client
    src/app
    src/io/clock.cpp
    src/io/errno.cpp
    src/io/fd.cpp
    src/io/poller.cpp
    src/io/tcp.cpp
    src/io/terminal.cpp
    src/io/include/zet/io/clock.hpp
    src/io/include/zet/io/errno.hpp
    src/io/include/zet/io/fd.hpp
    src/io/include/zet/io/poller.hpp
    src/io/include/zet/io/self_path.hpp
    src/io/include/zet/io/tcp.hpp
    src/io/include/zet/io/terminal.hpp
)

# One platform's calls and headers, and platform conditionals, which belong
# in poller_backend.hpp alone.
banned='(pipe2|accept4|SOCK_NONBLOCK|SOCK_CLOEXEC|epoll|signalfd|eventfd|pidfd|prctl|MSG_NOSIGNAL|kqueue|kevent|EVFILT_|__linux__|__APPLE__|sys/event\.h)'

files=$(find "${PORTABLE[@]}" -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | sort)
count=$(printf '%s\n' "$files" | grep -c .) || true

if [ "$count" -eq 0 ]; then
    echo "no sources on the portable path" >&2
    exit 2
fi

violations=0
for file in $files; do
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        printf '  %s:%s\n' "$file" "$hit"
        violations=$((violations + 1))
    # Block comments become blank lines of their own height, so that the
    # line numbers grep reports are still the file's.
    done < <(perl -0pe 's{/\*.*?\*/}{ (my $c = $&) =~ s/[^\n]//g; $c }gse; s{//[^\n]*}{}g' "$file" |
             grep -nE "$banned")
done

printf 'portable sources: %d, platform-specific uses: %d\n' "$count" "$violations"

if [ "$violations" -ne 0 ]; then
    echo
    echo "The client's path builds on Linux and macOS alike. Use the POSIX call"
    echo "and set flags with fcntl, or move the code into a backend file; see"
    echo "docs/design.md §10."
    exit 1
fi

echo "the client's path is portable"
