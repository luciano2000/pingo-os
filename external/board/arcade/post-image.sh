#!/usr/bin/env bash
# Pingo Arcade: partição de boot (firmware da Pi + kernel + overlay da Picade)
# e a .img. Executado pelo Buildroot a partir do diretório do Buildroot.
# $1 = BINARIES_DIR, $2 = diretório da placa (board/arcade)
set -euo pipefail
BOARD="$(realpath "$2")"
FW="$BINARIES_DIR/rpi-firmware"

# Overlay da Picade X HAT
mkdir -p "$FW/overlays"
"$HOST_DIR/bin/dtc" -@ -q -I dts -O dtb -o "$FW/overlays/pingo-picade.dtbo" "$BOARD/pingo-picade.dts"

files=()
for f in "$BINARIES_DIR"/*.dtb "$FW"/*; do
	files+=("${f#"$BINARIES_DIR"/}")
done
files+=(Image)
list=$(printf '\t\t\t"%s",\n' "${files[@]}")
cfg="$BINARIES_DIR/genimage-arcade.cfg"
awk -v l="$list" '/#BOOT_FILES#/ {print l; next} {print}' "$BOARD/genimage.cfg" > "$cfg"
support/scripts/genimage.sh -c "$cfg"
IMG="$BINARIES_DIR/pingo-arcade-rpi3.img"
xz -T0 -k -f "$IMG"
echo "Imagem pronta: $IMG (+ .xz)"
