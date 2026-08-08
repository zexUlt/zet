#!/usr/bin/env bash
# Сборка пресета в Alpine — на ноутбуке и в CI одной и той же командой.
#
# Использование: tools/musl_build.sh [preset] [ctest]
set -euo pipefail

preset=${1:-musl-release}
run_tests=${2:-}

build="cmake --preset $preset && cmake --build --preset $preset"
if [ -n "$run_tests" ]; then
    build="$build && ctest --preset $preset"
fi

exec "$(dirname "$0")/in_alpine.sh" "$build"
