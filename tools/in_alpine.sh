#!/usr/bin/env bash
# Выполняет команду в Alpine с установленным тулчейном.
#
# Тулчейн ставится из кешированных .apk, а не берётся из готового образа:
# базовый образ 3.5 МБ, установка из тёплого кеша — десять секунд без сети.
# Так же устроены были джобы под Ubuntu, которые кешировали .deb.
#
# Использование: tools/in_alpine.sh 'команда для shell'
set -euo pipefail

ALPINE_IMAGE=alpine:3.22

[ $# -gt 0 ] || { echo "нечего выполнять: tools/in_alpine.sh 'команда'" >&2; exit 2; }

root=$(cd "$(dirname "$0")/.." && pwd)
packages=$(grep -v '^#' "$root/tools/alpine-packages.txt" | tr '\n' ' ')
apk_cache=${ZET_APK_CACHE:-$root/build/.apk-cache}
mkdir -p "$apk_cache"

# Root внутри контейнера, потому что иначе apk не поставит пакеты. Результаты
# сборки возвращаются вызывающему в конце.
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
status=0
$* || status=\$?
chown -R \$ZET_UID:\$ZET_GID build /apk-cache 2>/dev/null || true
chmod -R a+rw /apk-cache 2>/dev/null || true
exit \$status
"
