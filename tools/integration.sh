#!/usr/bin/env bash
# Builds a preset and runs the integration suite against it, in Alpine with a
# real sshd, ssh, pty and iptables (design.md §16).
#
# The container gets NET_ADMIN and nothing more: iptables -j DROP is how the
# suite makes a half-open connection rather than a reset, and that needs the
# capability inside the container's own network namespace only.
#
# Usage: tools/integration.sh [preset] [pytest args...]
#
# Two variables point it at something else: ZET_PYTEST_TARGET replaces
# tests/integration (the M6 soak, tests/integration/soak_m6.py, is not
# collected by the directory run), and every ZET_SOAK_* in the environment is
# handed to the container for that soak.
set -euo pipefail

preset=${1:-musl-dev}
shift || true

root=$(cd "$(dirname "$0")/.." && pwd)

# Quoted for the container's sh: each pytest argument stays one word.
pytest_args=""
for arg in "$@"; do
    pytest_args="$pytest_args '${arg//\'/\'\\\'\'}'"
done

target=${ZET_PYTEST_TARGET:-tests/integration}

export ZET_EXTRA_PACKAGES=tools/integration-packages.txt
export ZET_DOCKER_ARGS="--cap-add=NET_ADMIN"
for name in $(compgen -e | grep '^ZET_SOAK_' || true); do
    ZET_DOCKER_ARGS="$ZET_DOCKER_ARGS -e $name"
done

exec "$root/tools/in_alpine.sh" "
cmake --preset $preset >/dev/null &&
cmake --build --preset $preset &&
python3 -m venv --system-site-packages /tmp/zet-venv &&
/tmp/zet-venv/bin/pip install --require-hashes -r /src/tools/pyte-requirements.txt &&
ZET_BUILD_DIR=/src/build/$preset /tmp/zet-venv/bin/python -m pytest -p no:cacheprovider -v $target $pytest_args
"
