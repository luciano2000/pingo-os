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

# Marca do Retrogamer Brasil para a galeria (logo e moeda)
install -D -m 0644 "$BR2_EXTERNAL_PINGO_PATH/board/arcade/marca/logo-horizontal.png" \
	"$TARGET_DIR/usr/share/pingo/arcade/logo-horizontal.png"
install -D -m 0644 "$BR2_EXTERNAL_PINGO_PATH/board/arcade/marca/logo-vertical.png" \
	"$TARGET_DIR/usr/share/pingo/arcade/logo-vertical.png"
install -D -m 0644 "$BR2_EXTERNAL_PINGO_PATH/board/arcade/marca/moeda.png" \
	"$TARGET_DIR/usr/share/pingo/arcade/moeda.png"

# SSH por chave: /root/.ssh no cartão (a raiz é somente leitura)
rm -rf "$TARGET_DIR/root/.ssh"
ln -sfn /mnt/sd1/.system/ssh "$TARGET_DIR/root/.ssh"

# Scripts que saíram do overlay (o Buildroot não apaga do target)
rm -f "$TARGET_DIR/etc/init.d/S95pingo-ip"
