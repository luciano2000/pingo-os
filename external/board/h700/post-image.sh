#!/usr/bin/env bash
# Monta a partição de boot e gera uma .img por aparelho com genimage.
# Executado pelo Buildroot a partir do diretório do Buildroot.
# $1 = BINARIES_DIR, $2 = diretório de uma placa (ex.: .../board/h700/rg35xxh)
#
# O kernel, o U-Boot e o sistema são os mesmos para todos os aparelhos H700;
# muda só o extlinux.conf (que device tree carregar). Então um build gera as
# imagens de todos: board/h700/<aparelho>/extlinux.conf ->
# pingo-os-<aparelho>.img
set -euo pipefail

H700_DIR="$(dirname "$(realpath "$2")")"

rm -rf "$BINARIES_DIR/dtbs"
mkdir -p "$BINARIES_DIR/dtbs"
cp "$BINARIES_DIR"/*.dtb "$BINARIES_DIR/dtbs/"
rm -f "$BINARIES_DIR/pingo-wifi.txt" # Wi-Fi agora pela tela do launcher

for board in "$H700_DIR"/*/; do
	[ -f "$board/extlinux.conf" ] || continue
	dev="$(basename "$board")"
	rm -rf "$BINARIES_DIR/extlinux"
	mkdir -p "$BINARIES_DIR/extlinux"
	cp "$board/extlinux.conf" "$BINARIES_DIR/extlinux/extlinux.conf"
	# a configuração do genimage com o nome deste aparelho
	cfg="$BINARIES_DIR/genimage-$dev.cfg"
	sed -e "s/pingo-os-rg35xxh\.img/pingo-os-$dev.img/" \
	    -e "s/boot\.vfat/boot-$dev.vfat/g" \
	    "$H700_DIR/genimage.cfg" > "$cfg"
	support/scripts/genimage.sh -c "$cfg"
	IMG="$BINARIES_DIR/pingo-os-$dev.img"
	xz -T0 -k -f "$IMG"
	echo "Imagem pronta: $IMG (+ .xz)"
done
