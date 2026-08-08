#!/usr/bin/env bash
# Builds inside Alpine, so a laptop and CI run the same compiler against the
# same libc. What comes out is what ships.
#
# The toolchain is installed from cached .apk files rather than baked into an
# image: the base image is 3.5 MB, and installing from a warm cache takes ten
# seconds with the network switched off. Same shape as the Ubuntu jobs, which
# cache .deb rather than an image.
#
# Usage: tools/musl_build.sh [preset] [ctest]
set -euo pipefail

ALPINE_IMAGE=alpine:3.22

preset=${1:-musl-release}
run_tests=${2:-}

root=$(cd "$(dirname "$0")/.." && pwd)
packages=$(grep -v '^#' "$root/tools/alpine-packages.txt" | tr '\n' ' ')
apk_cache=${ZET_APK_CACHE:-$root/build/.apk-cache}
mkdir -p "$apk_cache"

# Root inside the container, because apk needs it — the build products are
# handed back to the caller at the end instead.
args=(
    --rm
    -v "$root:/src"
    -v "$apk_cache:/apk-cache"
    -w /src
    -e "ZET_UID=$(id -u)"
    -e "ZET_GID=$(id -g)"
)
if [ -n "${CCACHE_DIR:-}" ]; then
    mkdir -p "$CCACHE_DIR"
    args+=(-v "$CCACHE_DIR:/ccache" -e CCACHE_DIR=/ccache -e ZET_COMPILER_LAUNCHER=ccache)
fi

build="cmake --preset $preset && cmake --build --preset $preset"
if [ -n "$run_tests" ]; then
    build="$build && ctest --preset $preset"
fi

docker run "${args[@]}" "$ALPINE_IMAGE" sh -c "
set -e
apk add --no-progress --cache-dir /apk-cache --update-cache $packages >/dev/null
$build
chown -R \$ZET_UID:\$ZET_GID build /apk-cache
chmod -R a+rw /apk-cache
"
