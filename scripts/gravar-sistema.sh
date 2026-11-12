#!/bin/sh
# Grava só as partições de boot (1) e de sistema (2) de uma imagem do Pingo
# num cartão que já tem o Pingo, mantendo a partição ROMS (jogos e saves).
#
# Uso: sudo scripts/gravar-sistema.sh <imagem.img> </dev/sdX>
#
# Os endereços vêm da tabela de partições da própria imagem, e o script só
# grava se as partições 1 e 2 do cartão começarem e tiverem o tamanho das da
# imagem (o mesmo aparelho, mesmo layout). Nada de números digitados à mão.
set -eu

IMG=${1:-}
DISK=${2:-}
[ -f "$IMG" ] && [ -b "$DISK" ] || { echo "uso: $0 <imagem.img> </dev/sdX>" >&2; exit 2; }
case "$DISK" in *[0-9]) P=p ;; *) P= ;; esac

for n in 1 2; do
	i=$(sfdisk -d "$IMG" | grep -E "^[^ ]*[^0-9]$n :" | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/')
	d=$(sfdisk -d "$DISK" | grep -E "^${DISK}${P}$n :" | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/')
	[ -n "$i" ] || { echo "imagem sem partição $n" >&2; exit 1; }
	if [ "$i" != "$d" ]; then
		echo "partição $n diferente (imagem: $i, cartão: ${d:-nenhuma}): é outro aparelho ou outro layout; não gravo" >&2
		exit 1
	fi
	eval "start$n=${i% *} size$n=${i#* }"
done

for n in 1 2; do
	dev=$DISK$P$n
	if grep -q "^$dev " /proc/mounts; then
		umount "$dev" || { echo "não consegui desmontar $dev" >&2; exit 1; }
	fi
done

for n in 1 2; do
	eval "s=\$start$n c=\$size$n"
	echo "partição $n: setor $s, $((c / 2048)) MiB"
	dd if="$IMG" of="$DISK$P$n" bs=4M iflag=skip_bytes,count_bytes \
		skip=$((s * 512)) count=$((c * 512)) conv=fsync status=progress
done
sync
echo GRAVADO
