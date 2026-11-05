#!/usr/bin/env python3
"""DAT do FBNeo (ClrMame Pro XML) -> tabela de nomes para o launcher.

Uso: fbneo-names.py <dat> <saida.tsv>

Uma linha por jogo: "<nome do zip>\t<descrição>\t<1 se é BIOS>". Só a
biblioteca padrão (o build roda num contêiner sem extras).
"""
import sys
import xml.etree.ElementTree as ET


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root = ET.parse(sys.argv[1]).getroot()
    rows = []
    for g in root.iter("game"):
        name = g.get("name")
        desc = (g.findtext("description") or name).replace("\t", " ").strip()
        bios = 1 if g.get("isbios") == "yes" else 0
        rows.append(f"{name}\t{desc}\t{bios}")
    rows.sort()
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    print(f"{sys.argv[2]}: {len(rows)} jogos")


if __name__ == "__main__":
    main()
