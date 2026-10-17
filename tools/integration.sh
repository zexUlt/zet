#!/usr/bin/env bash
# Builds a preset and runs the integration suite against it, in Alpine with a
# real sshd, ssh, pty and iptables (design.md §16).
#
# The container gets NET_ADMIN and nothing more: iptables -j DROP is how the
# suite makes a half-open connection rather than a reset, and that needs the
# capability inside the container's own network namespace only.
#
# Usage: tools/integration.sh [preset] [pytest args...]
set -euo pipefail

preset=${1:-musl-dev}
shift || true

root=$(cd "$(dirname "$0")/.." && pwd)

# Quoted for the container's sh: each pytest argument stays one word.
pytest_args=""
for arg in "$@"; do
    pytest_args="$pytest_args '${arg//\'/\'\\\'\'}'"
done

export ZET_EXTRA_PACKAGES=tools/integration-packages.txt
export ZET_DOCKER_ARGS="--cap-add=NET_ADMIN"

exec "$root/tools/in_alpine.sh" "
cmake --preset $preset >/dev/null &&
cmake --build --preset $preset &&
ZET_BUILD_DIR=/src/build/$preset python3 -m pytest -p no:cacheprovider -v tests/integration $pytest_args
"
