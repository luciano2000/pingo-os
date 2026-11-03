#!/usr/bin/env bash
# Monta o conteúdo da partição de boot e gera a .img com genimage.
# Executado pelo Buildroot a partir do diretório do Buildroot.
# $1 = BINARIES_DIR, $2 = diretório da placa (ex.: .../board/h700/rg35xxh)
set -euo pipefail

BOARD_DIR="$(realpath "$2")"
H700_DIR="$(dirname "$BOARD_DIR")"

rm -rf "$BINARIES_DIR/extlinux" "$BINARIES_DIR/dtbs"
mkdir -p "$BINARIES_DIR/extlinux" "$BINARIES_DIR/dtbs"
cp "$BOARD_DIR/extlinux.conf" "$BINARIES_DIR/extlinux/extlinux.conf"
cp "$BINARIES_DIR"/*.dtb "$BINARIES_DIR/dtbs/"
cp "$H700_DIR/pingo-wifi.txt" "$BINARIES_DIR/pingo-wifi.txt"

support/scripts/genimage.sh -c "$H700_DIR/genimage.cfg"

IMG="$BINARIES_DIR/pingo-os-$(basename "$BOARD_DIR").img"
xz -T0 -k -f "$IMG"
echo "Imagem pronta: $IMG (+ .xz)"
