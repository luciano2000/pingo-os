#!/usr/bin/env bash
# Build do Pingo OS dentro do Docker.
# Uso: scripts/build.sh [aparelho] [alvo-make...]
#   scripts/build.sh                 -> rg35xxh, imagem completa
#   scripts/build.sh rg35xxh menuconfig
#   scripts/build.sh rg35xxh linux-rebuild all
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BR_VERSION=2026.08.1
DEVICE="${1:-rg35xxh}"; shift || true
TARGETS=("$@")

BR_TAR="$ROOT/.cache/buildroot-$BR_VERSION.tar.xz"
BR_DIR="$ROOT/.cache/buildroot-$BR_VERSION"
OUT="$ROOT/output/$DEVICE"
IMAGE=pingo-builder:trixie

mkdir -p "$ROOT/.cache/dl" "$OUT"
if [ ! -d "$BR_DIR" ]; then
  [ -f "$BR_TAR" ] || curl -fL "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz" -o "$BR_TAR"
  sha256sum -c <<<"$(cat "$ROOT/scripts/buildroot.sha256")  $BR_TAR"
  tar -xJf "$BR_TAR" -C "$ROOT/.cache"
fi

docker build -q -t "$IMAGE" "$ROOT/docker" >/dev/null

run() {
  docker run --rm -i $([ -t 0 ] && echo -t) \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e BR2_DL_DIR="$ROOT/.cache/dl" \
    -v "$ROOT:$ROOT" -w "$BR_DIR" "$IMAGE" "$@"
}

if [ ! -f "$OUT/.config" ]; then
  run make O="$OUT" BR2_EXTERNAL="$ROOT/external" "pingo_${DEVICE}_defconfig"
fi
run make O="$OUT" "${TARGETS[@]:-all}"
