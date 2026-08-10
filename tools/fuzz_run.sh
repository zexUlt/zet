#!/usr/bin/env bash
# Builds every libFuzzer target and runs each one for a fixed time.
#
# The list of targets comes from the manifest CMake generates, not from this
# script and not from the workflow: adding a fuzzer is one line in
# fuzz/CMakeLists.txt and nothing anywhere else. ET's fuzzers are named in a
# CMakeLists that points at files which do not exist, and CI never runs them —
# the failure mode this arrangement is built to make impossible.
#
# Seeds are committed as hex text so that a corpus stays reviewable in a diff;
# they are decoded into the build directory here. Anything in a corpus that is
# not .hex is a crash input kept byte for byte and is copied as it is.
#
# Usage: tools/fuzz_run.sh [seconds] [preset]
set -euo pipefail

seconds=${1:-60}
preset=${2:-musl-fuzz}
build=build/$preset

# 8 KiB: enough for a body that clears MAX_HANDSHAKE_FRAME, which is where the
# two stage limits disagree. Left to itself libFuzzer would take the length of
# the largest seed, and the seeds are tens of bytes.
readonly MAX_INPUT_LENGTH=8192

# A single input has no reason to take longer than this; anything that does is
# a finding in its own right.
readonly INPUT_TIMEOUT_SECONDS=25

# LeakSanitizer cannot read the main thread's stack under musl: it reported the
# vector libFuzzer allocates in its own main() as leaked, symbolising the frame
# below it as "[stack]". Every run would end in a false finding, which is worse
# than no leak detection at all — a gate that cries wolf gets switched off for
# real. The core allocates nothing that outlives a call anyway; ASan and UBSan
# stay on.
#
# Both switches are needed: libFuzzer's own flag governs the check it runs
# between inputs, ASAN_OPTIONS the one ASan runs at exit, and it was the latter
# that failed the run.
readonly DETECT_LEAKS=0
export ASAN_OPTIONS="detect_leaks=$DETECT_LEAKS"

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

# The sanitizer runtimes and the clang that produced the instrumentation live
# in the container, so the whole run happens inside it.
if [ -z "${ZET_IN_ALPINE:-}" ]; then
    exec "$root/tools/in_alpine.sh" \
        "ZET_IN_ALPINE=1 tools/fuzz_run.sh $seconds $preset"
fi

cmake --preset "$preset" >/dev/null
cmake --build --preset "$preset" >/dev/null

manifest=$build/fuzz/fuzzers.txt
if [ ! -s "$manifest" ]; then
    echo "no fuzzers listed in $manifest — none were built" >&2
    exit 2
fi

findings=$build/findings
rm -rf "$findings"
mkdir -p "$findings"

status=0
count=0

while read -r name binary corpus; do
    [ -n "$name" ] || continue
    count=$((count + 1))

    seeds=$build/seeds/$name
    rm -rf "$seeds"
    mkdir -p "$seeds"
    for seed in "$corpus"/*; do
        [ -e "$seed" ] || continue
        case "$seed" in
        *.hex)
            hex=$(tr -cd '0-9a-fA-F' <"$seed")
            printf '%b' "$(printf '%s' "$hex" | sed 's/../\\x&/g')" \
                >"$seeds/$(basename "${seed%.hex}")"
            ;;
        *)
            cp "$seed" "$seeds/"
            ;;
        esac
    done

    # The first directory is the one libFuzzer writes what it keeps into, so it
    # is a scratch one under build/. The committed corpus is an input and stays
    # exactly as it was checked in.
    work=$build/work/$name
    mkdir -p "$work"

    echo "== $name for ${seconds}s"
    if ! "$binary" "$work" "$seeds" \
        -max_total_time="$seconds" \
        -max_len="$MAX_INPUT_LENGTH" \
        -timeout="$INPUT_TIMEOUT_SECONDS" \
        -detect_leaks="$DETECT_LEAKS" \
        -print_final_stats=1 \
        -artifact_prefix="$findings/$name-"; then
        echo "FAIL $name"
        status=1
    fi
done <"$manifest"

printf 'fuzzers run: %d, with findings: %d\n' "$count" "$status"

if [ "$status" -ne 0 ]; then
    echo
    echo "A fuzzer found an input. The artifact under $findings reproduces it:"
    echo "run the target with that file as its only argument. Every finding"
    echo "goes into fuzz/corpus/ as a regression seed and stays there."
fi

exit "$status"
