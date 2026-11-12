#!/usr/bin/env python3
"""Drivers do FBNeo (src/burn/drv/**/*.cpp) -> gênero, jogadores e marcas.

Uso: fbneo-drivers.py <src/burn> <saida.tsv>

Lê cada "struct BurnDriver BurnDrvXxx = { ... };" e grava, separado por TAB:
  zip  generos  jogadores  marcas

- generos: os GBF_* sem o prefixo, separados por vírgula (RUNGUN,PLATFORM)
- marcas: dos BDF_*: funciona, vertical, clone, bootleg, prototipo, hack,
  homebrew, demo

O fbneo-arcade.py junta isto ao DAT. Só a biblioteca padrão.
"""
import os
import re
import sys

START = re.compile(r"struct\s+BurnDriver\s+BurnDrv\w+\s*=\s*\{")
FLAGS = {
    "BDF_GAME_WORKING": "funciona", "BDF_ORIENTATION_VERTICAL": "vertical",
    "BDF_CLONE": "clone", "BDF_BOOTLEG": "bootleg", "BDF_PROTOTYPE": "prototipo",
    "BDF_HACK": "hack", "BDF_HOMEBREW": "homebrew", "BDF_DEMO": "demo",
}


def fields(body):
    """Divide o inicializador nas vírgulas de nível zero (fora de aspas)."""
    out, cur, depth, i, quote = [], [], 0, 0, None
    while i < len(body):
        c = body[i]
        if quote:
            cur.append(c)
            if c == "\\":
                cur.append(body[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c == "/" and body[i:i + 2] == "//":
            j = body.find("\n", i)
            i = len(body) if j < 0 else j
            continue
        elif c == "/" and body[i:i + 2] == "/*":
            j = body.find("*/", i)
            i = len(body) if j < 0 else j + 2
            continue
        elif c in "({":
            depth += 1
            cur.append(c)
        elif c in ")}":
            depth -= 1
            cur.append(c)
        elif c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def parse_file(path, rows):
    with open(path, encoding="latin-1") as f:
        src = f.read()
    for m in START.finditer(src):
        end = src.find("};", m.end())
        if end < 0:
            continue
        fl = fields(src[m.end():end])
        if len(fl) < 18:
            continue
        name = fl[0].strip('"')
        if not re.fullmatch(r"[\w]+", name):
            continue
        flags, players, genre = fl[13], fl[14], fl[16]
        marks = [v for k, v in FLAGS.items() if re.search(r"\b" + k + r"\b", flags)]
        genres = re.findall(r"GBF_(\w+)", genre)
        try:
            players = str(int(players, 0))
        except ValueError:
            players = ""
        rows[name] = "\t".join([name, ",".join(genres), players, ",".join(marks)])


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rows = {}
    for d, _, files in os.walk(os.path.join(sys.argv[1], "drv")):
        for fn in files:
            if fn.endswith(".cpp"):
                parse_file(os.path.join(d, fn), rows)
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("\n".join(rows[k] for k in sorted(rows)) + "\n")
    print(f"{sys.argv[2]}: {len(rows)} drivers")


if __name__ == "__main__":
    main()
