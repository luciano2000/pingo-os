#!/usr/bin/env bash
# Pingo Arcade. $1 = TARGET_DIR
set -euo pipefail
TARGET_DIR="$1"

VERSION="$(cd "$BR2_EXTERNAL_PINGO_PATH" && git describe --always --dirty 2>/dev/null || echo dev)"
cat > "$TARGET_DIR/etc/os-release" <<OS
NAME="Pingo Arcade"
PRETTY_NAME="Pingo Arcade by Retrogamer Brasil"
ID=pingo-arcade
VERSION_ID="$VERSION"
HOME_URL="https://github.com/luciano2000/pingo-os"
OS

mkdir -p "$TARGET_DIR/boot" "$TARGET_DIR/mnt/sd1"

# Fonte das listas (Quicksand, OFL): a mesma do Pingo OS, que está no
# overlay do H700
mkdir -p "$TARGET_DIR/usr/share/fonts/quicksand"
cp "$BR2_EXTERNAL_PINGO_PATH"/board/h700/rootfs-overlay/usr/share/fonts/quicksand/* \
	"$TARGET_DIR/usr/share/fonts/quicksand/"
