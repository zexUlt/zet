#!/usr/bin/env bash
# Invariant 4: zet never phones home.
#
# The guarantee is that the code to send anything does not exist, so this looks
# for outbound network primitives in the shipped binaries and allows them only
# in the transport module. Contrast: EternalTerminal ships a Sentry DSN and a
# Datadog API key compiled into the binary, on by default in the client.
#
# Usage: tools/check_no_telemetry.sh [build-dir]
set -uo pipefail

build=${1:-build/release}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

command -v nm >/dev/null 2>&1 || { echo "нет nm, гейт пропущен" >&2; exit 2; }

# Anything that could carry data off the machine on its own.
banned='^(curl_easy_init|curl_easy_perform|SSL_connect|SSL_write|gnutls_handshake|sentry_init|sentry_capture_event)$'

# Objects outside the transport module: nothing here may resolve a hostname or
# open a socket. zet_io owns all of that.
mapfile -t objects < <(find "$build" -name '*.o' \
    -not -path '*zet_io.dir*' -not -path '*_deps*' 2>/dev/null | sort)

if [ ${#objects[@]} -eq 0 ]; then
    echo "в $build нет объектных файлов — собери сначала" >&2
    exit 2
fi

violations=0
for obj in "${objects[@]}"; do
    while read -r sym; do
        [ -n "$sym" ] || continue
        printf '  %s -> %s\n' "${obj#"$build"/}" "$sym"
        violations=$((violations + 1))
    done < <(nm --undefined-only --format=posix "$obj" 2>/dev/null |
             awk '{print $1}' | sed 's/@.*//' | grep -E "$banned")
done

printf 'объектов вне zet_io: %d, исходящих сетевых примитивов: %d\n' \
    "${#objects[@]}" "$violations"

if [ "$violations" -ne 0 ]; then
    echo
    echo "Ничего, кроме транспорта, не имеет права ходить в сеть."
    echo "Телеметрии в zet нет и не будет; см. docs/design.md §15."
    exit 1
fi

echo "исходящих сетевых вызовов вне транспорта нет"
