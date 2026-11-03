#!/usr/bin/env bash
# $1 = TARGET_DIR
set -euo pipefail
TARGET_DIR="$1"

VERSION="$(cd "$BR2_EXTERNAL_PINGO_PATH" && git describe --always --dirty 2>/dev/null || echo dev)"
cat > "$TARGET_DIR/etc/os-release" <<OS
NAME="Pingo OS"
PRETTY_NAME="Pingo OS by Retrogamer Brasil"
ID=pingo
VERSION_ID="$VERSION"
HOME_URL="https://github.com/luciano2000/pingo-os"
OS

# Pontos de montagem: boot (FAT) e cartões
mkdir -p "$TARGET_DIR/boot" "$TARGET_DIR/mnt/sd1" "$TARGET_DIR/mnt/sd2"
