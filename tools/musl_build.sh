#!/usr/bin/env bash
# Builds a preset in Alpine — the same command on a laptop and in CI.
#
# Usage: tools/musl_build.sh [preset] [ctest]
set -euo pipefail

preset=${1:-musl-release}
run_tests=${2:-}

build="cmake --preset $preset && cmake --build --preset $preset"
if [ -n "$run_tests" ]; then
    build="$build && ctest --preset $preset"
fi

exec "$(dirname "$0")/in_alpine.sh" "$build"
