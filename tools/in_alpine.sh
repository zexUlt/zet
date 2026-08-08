#!/usr/bin/env bash
# Runs a command in Alpine with the toolchain installed.
#
# The toolchain comes from cached .apk files rather than from a prebuilt image:
# the base image is 3.5 MB, and installing from a warm cache takes ten seconds
# with no network. The Ubuntu jobs that cached .deb files worked the same way.
#
# Usage: tools/in_alpine.sh 'shell command'
set -euo pipefail

ALPINE_IMAGE=alpine:3.22

[ $# -gt 0 ] || { echo "nothing to run: tools/in_alpine.sh 'command'" >&2; exit 2; }

root=$(cd "$(dirname "$0")/.." && pwd)
packages=$(grep -v '^#' "$root/tools/alpine-packages.txt" | tr '\n' ' ')
apk_cache=${ZET_APK_CACHE:-$root/build/.apk-cache}
mkdir -p "$apk_cache"

# Root inside the container, because apk will not install packages otherwise.
# The build results are handed back to the caller at the end.
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

docker run "${args[@]}" "$ALPINE_IMAGE" sh -c "
set -e
apk add --no-progress --cache-dir /apk-cache --update-cache $packages >/dev/null

# The command gets its own lines instead of an '||': a multi-line argument
# would carry the operator onto a new line and sh would fail on the syntax.
set +e
$*
status=\$?
set -e
chown -R \$ZET_UID:\$ZET_GID build /apk-cache 2>/dev/null || true
chmod -R a+rw /apk-cache 2>/dev/null || true
exit \$status
"
