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

# PortMaster: ele e os ports procuram /roms/ports; aqui é a pasta Ports do SD1
mkdir -p "$TARGET_DIR/roms"
ln -sfn /mnt/sd1/Ports "$TARGET_DIR/roms/ports"

# O Mesa precisa do Clang/libclc só no build (shaders internos pré-compilados)
# e para OpenCL, que não usamos. O libgallium liga só no libLLVM.
rm -f "$TARGET_DIR"/usr/lib/libclang*.so* "$TARGET_DIR"/usr/lib/libLLVMSPIRVLib.so*
rm -rf "$TARGET_DIR"/usr/share/clc
rm -f "$TARGET_DIR"/usr/bin/llvm-spirv "$TARGET_DIR"/usr/bin/spirv-*

# Tela de boot: PNG -> formato bruto do framebuffer (XRGB8888 640x480)
mkdir -p "$TARGET_DIR/usr/share/pingo"
python3 -I "$BR2_EXTERNAL_PINGO_PATH/../scripts/png2fb.py" \
	"$BR2_EXTERNAL_PINGO_PATH/board/h700/splash/pingo.png" \
	"$TARGET_DIR/usr/share/pingo/splash.raw"
