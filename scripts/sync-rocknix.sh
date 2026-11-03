#!/usr/bin/env bash
# Importa do ROCKNIX (GPL-2.0) os patches, device trees e firmwares do H700.
# Uso: scripts/sync-rocknix.sh [commit]
# O commit fica registrado em external/board/h700/ROCKNIX_COMMIT.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMIT="${1:-$(cat "$ROOT/external/board/h700/ROCKNIX_COMMIT" 2>/dev/null || echo next)}"
REPO=https://github.com/ROCKNIX/distribution.git
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git -C "$WORK" init -q
git -C "$WORK" remote add origin "$REPO"
git -C "$WORK" fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
git -C "$WORK" sparse-checkout set --no-cone \
  /projects/ROCKNIX/devices/H700/ \
  /projects/ROCKNIX/packages/tools/atf/patches/H700/ \
  /projects/ROCKNIX/packages/linux-firmware/kernel-firmware/extra-firmware/panels/ \
  /projects/ROCKNIX/packages/linux/patches/mainline/ \
  /projects/ROCKNIX/packages/linux/patches/7.2/
git -C "$WORK" checkout -q FETCH_HEAD
SHA="$(git -C "$WORK" rev-parse HEAD)"

R="$WORK/projects/ROCKNIX"
D="$R/devices/H700"
B="$ROOT/external/board/h700"
P="$B/patches"

rm -rf "$P/linux" "$P/uboot" "$P/arm-trusted-firmware" "$B/dts" "$B/firmware/panels" "$B/uboot"
mkdir -p "$P/linux" "$P/uboot" "$P/arm-trusted-firmware" "$B/dts" "$B/firmware/panels" "$B/uboot"

# Kernel 7.2. O ROCKNIX aplica primeiro os patches do pacote (dirs "7.2" e "mainline")
# e depois os do aparelho. O Buildroot aplica em ordem alfabética, então os do pacote
# recebem o prefixo 0000- para manter a mesma ordem. Os patches de 7.2 são quase todos
# de Qualcomm (msm); só o 9998 é genérico.
for f in "$R"/packages/linux/patches/mainline/*.patch; do cp "$f" "$P/linux/0000-mainline-$(basename "$f")"; done
cp "$R/packages/linux/patches/7.2/9998-silence-initramfs-unpack-warn.patch" "$P/linux/0000-7.2-9998-silence-initramfs-unpack-warn.patch"
cp "$D"/patches/linux/*.patch "$P/linux/"
cp "$D/linux/linux.aarch64.conf" "$B/linux-rocknix.config"
cp -r "$D/linux/dts/allwinner" "$B/dts/"

# U-Boot (LPDDR4: RG35XX-H, RG40XX-H/V)
cp "$D"/packages/u-boot-DDR4/patches/*.patch "$P/uboot/"
cp "$D"/packages/u-boot-DDR4/sources/configs/*_defconfig "$B/uboot/"

# TF-A: PSCI SYSTEM_SUSPEND via stub em SRAM (ativado mais tarde, Fase 0 item 4)
cp "$R"/packages/tools/atf/patches/H700/*.patch "$P/arm-trusted-firmware/"

# Sequências de inicialização dos painéis (driver panel-mipi-dpi-spi)
cp "$R"/packages/linux-firmware/kernel-firmware/extra-firmware/panels/*.panel "$B/firmware/panels/"

echo "$SHA" > "$B/ROCKNIX_COMMIT"
echo "ROCKNIX $SHA importado em $B"
