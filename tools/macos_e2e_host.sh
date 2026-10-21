#!/usr/bin/env bash
# The test host for end-to-end runs of a client outside Linux (design.md §16):
# a long-lived Alpine container with sshd, the test user and zet-agent, on a
# machine with docker — colima on a Mac, locally or on the macos-15-intel
# runner.
#
# The agent binds an ephemeral port and announces it in BootstrapAccept, and
# the client connects to that port on the address ssh resolves to. The
# container narrows its ephemeral range (net.ipv4.ip_local_port_range, a
# per-namespace sysctl) to AGENT_PORTS and publishes that range on the host
# with the same numbers, so whatever port the agent picks is reachable as is.
# No test-only switch in the agent: it does what it does on any host.
#
#     tools/macos_e2e_host.sh up      build zet-agent, start the host, wait
#     tools/macos_e2e_host.sh down    stop and remove it
#
# With ZET_E2E_AGENT set to a zet-agent built elsewhere — CI builds it once on
# Linux and hands it over as an artifact — `up` skips the build and the
# toolchain: the agent is static, and the host needs only what the tests run.
#
# `up` leaves build/macos-e2e/ssh_config and build/macos-e2e/host.json for the
# client side: tests/macos_e2e reads the latter.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

readonly CONTAINER=${ZET_E2E_CONTAINER:-zet-e2e-host}
readonly SSH_PORT=${ZET_E2E_SSH_PORT:-42222}
readonly AGENT_PORT_FIRST=${ZET_E2E_AGENT_PORT_FIRST:-42300}
# Fifty: one per agent, and agents of failed tests linger until their orphan
# timeout.
readonly AGENT_PORT_COUNT=50
readonly AGENT_PORT_LAST=$((AGENT_PORT_FIRST + AGENT_PORT_COUNT - 1))
readonly ALPINE_IMAGE=alpine:3.22
readonly OUT=build/macos-e2e
# A cold build of zet-agent on a 4-core runner, with room to spare. The wait
# is on the container's log, so a ready host is seen at once.
readonly READY_TIMEOUT_SECONDS=1800
readonly READY_LINE="zet-e2e-host ready"

down() {
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
}

up() {
    down
    rm -rf "$OUT"
    mkdir -p "$OUT"
    local apk_cache=${ZET_APK_CACHE:-$root/build/.apk-cache}
    mkdir -p "$apk_cache"
    local host_packages
    host_packages="$(grep -v '^#' tools/integration-packages.txt | tr '\n' ' ')"

    local common=(
        -v "$root:/src" -v "$apk_cache:/apk-cache" -w /src
        -e "ZET_UID=$(id -u)" -e "ZET_GID=$(id -g)"
    )
    if [ -n "${CCACHE_DIR:-}" ]; then
        mkdir -p "$CCACHE_DIR"
        common+=(-v "$CCACHE_DIR:/ccache" -e CCACHE_DIR=/ccache -e ZET_COMPILER_LAUNCHER=ccache)
    fi
    local apk="apk add --no-progress --cache-dir /apk-cache --update-cache"
    local install="$apk $host_packages >/dev/null"

    # When ZET_E2E_AGENT is set the caller provides a pre-built binary and the
    # host starts with that agent pre-installed (legacy / local development).
    # Without it the host starts with no agent: the client delivers it over ssh
    # at connect time (M5.5 delivery mode, what CI uses).
    local agent_arg=""
    if [ -n "${ZET_E2E_AGENT:-}" ]; then
        # Under the checkout, which is what the container mounts.
        mkdir -p "$OUT/agent"
        cp "$ZET_E2E_AGENT" "$OUT/agent/zet-agent"
        chmod 755 "$OUT/agent/zet-agent"
        agent_arg="--agent /src/$OUT/agent/zet-agent"
    fi

    local args=(
        -d --init --name "$CONTAINER"
        --cap-add=NET_ADMIN
        --sysctl "net.ipv4.ip_local_port_range=$AGENT_PORT_FIRST $AGENT_PORT_LAST"
        -p "127.0.0.1:$SSH_PORT:2222"
        -p "127.0.0.1:$AGENT_PORT_FIRST-$AGENT_PORT_LAST:$AGENT_PORT_FIRST-$AGENT_PORT_LAST"
        "${common[@]}"
    )
    docker run "${args[@]}" "$ALPINE_IMAGE" sh -c "
set -e
$install
cd tests/integration
exec python3 host.py $agent_arg --out /src/$OUT \
    --client-dir '$root/$OUT' --client-port $SSH_PORT \
    --agent-ports $AGENT_PORT_FIRST-$AGENT_PORT_LAST --container $CONTAINER
" >/dev/null

    # Follows the log until the ready line, the container's exit, or the
    # deadline, whichever comes first. read -t is the deadline: no polling.
    local deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
    local line ready=0
    while :; do
        local left=$((deadline - SECONDS))
        [ "$left" -gt 0 ] || break
        IFS= read -r -t "$left" line || break
        printf '%s\n' "$line"
        if [ "$line" = "$READY_LINE" ]; then
            ready=1
            break
        fi
    done < <(docker logs -f "$CONTAINER" 2>&1)

    if [ "$ready" -ne 1 ]; then
        echo "the host did not come up; its log:" >&2
        docker logs "$CONTAINER" 2>&1 | tail -40 >&2
        down
        exit 1
    fi
    echo "ssh -F $root/$OUT/ssh_config zet-test-host"
    echo "description: $root/$OUT/host.json"
}

case "${1:-}" in
    up) up ;;
    down) down ;;
    *) echo "usage: tools/macos_e2e_host.sh up|down" >&2; exit 2 ;;
esac
