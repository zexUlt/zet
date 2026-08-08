#!/usr/bin/env bash
# Builds inside the Alpine toolchain image, so a laptop and CI run the same
# compiler against the same libc. What comes out is what ships.
#
# Usage: tools/musl_build.sh [preset] [ctest]
set -euo pipefail

preset=${1:-musl-release}
run_tests=${2:-}

root=$(cd "$(dirname "$0")/.." && pwd)
image=zet-musl:local

docker build -q -t "$image" -f "$root/docker/musl.Dockerfile" "$root/docker" >/dev/null

# --user keeps the build products owned by the caller instead of root.
# The ccache directory is passed through so the container hits the same cache
# the host job restored.
args=(
    --rm
    --user "$(id -u):$(id -g)"
    -v "$root:/src"
    -w /src
)
if [ -n "${CCACHE_DIR:-}" ]; then
    mkdir -p "$CCACHE_DIR"
    args+=(-v "$CCACHE_DIR:/ccache" -e CCACHE_DIR=/ccache -e ZET_COMPILER_LAUNCHER=ccache)
fi

script="cmake --preset $preset && cmake --build --preset $preset"
if [ -n "$run_tests" ]; then
    script="$script && ctest --preset $preset"
fi

docker run "${args[@]}" "$image" bash -c "$script"
