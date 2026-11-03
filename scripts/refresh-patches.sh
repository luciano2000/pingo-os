#!/usr/bin/env bash
# Regenera uma série de patches para que aplique sem fuzz (o Buildroot usa patch -F0).
# Aplica cada patch como o ROCKNIX faz (fuzz padrão) sobre a fonte limpa e grava o
# diff exato, mantendo o cabeçalho (From/Subject) original.
# Uso: scripts/refresh-patches.sh <tarball-da-fonte> <dir-de-patches>
set -euo pipefail
TARBALL="$(realpath "$1")"; PDIR="$(realpath "$2")"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

tar -xf "$TARBALL" -C "$WORK"
SRC="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d | head -1)"
cd "$SRC"
git init -q && git add -A && git -c user.name=r -c user.email=r@r commit -q -m base

for p in $(ls "$PDIR"/*.patch | sort); do
  if ! patch -p1 -s --no-backup-if-mismatch < "$p" >/tmp/refresh.$$ 2>&1; then
    echo "FALHOU: $(basename "$p")"; cat /tmp/refresh.$$; exit 1
  fi
  find . -name '*.orig' -delete
  git add -A
  # cabeçalho = tudo antes do primeiro "diff " ou "--- "
  awk '/^(diff |--- )/{exit} {print}' "$p" > "$p.new"
  git -c core.abbrev=12 diff --cached --no-color >> "$p.new"
  if cmp -s "$p" "$p.new"; then rm "$p.new"; else mv "$p.new" "$p"; echo "atualizado: $(basename "$p")"; fi
  git -c user.name=r -c user.email=r@r commit -q -m "$(basename "$p")"
done
rm -f /tmp/refresh.$$
echo "ok: todos os patches aplicam sem fuzz"
