#!/usr/bin/env python3
"""DAT do FBNeo (ClrMame Pro XML) -> tabela da galeria do Pingo Arcade.

Uso: fbneo-arcade.py <dat> <src/burn> <saida.tsv>

Uma linha por jogo, separada por TAB:
  zip  nome  ano  fabricante  pai  sistema  tipo  generos  jogadores  vertical

- pai: o zip do jogo original, se este for clone (senão vazio)
- sistema: a pasta do driver no FBNeo (neogeo, capcom, cave, ...)
- tipo: "jogo" ou o motivo de ficar fora dos Clássicos: bios, homebrew,
  hack, bootleg, aposta, quiz, pcb (versão de placa repetida), quebrado
  (o FBNeo marca como não funcionando)
- generos, jogadores, vertical: do código dos drivers (fbneo-drivers.py)

Só a biblioteca padrão (o build roda num contêiner sem extras).
"""
import importlib
import os
import re
import sys
import xml.etree.ElementTree as ET

# Marcados como bootleg/hack/conversão no DAT, mas são jogos de verdade
# (às vezes a única versão que existe)
KEEP = {"119", "empcity", "fspiderb", "moguchan", "dariusgx", "8ballact",
        "drakton", "drivfrcp", "reaktor"}

HOMEBREW = re.compile(r"\(HB\b|\bHB,|homebrew|\bdemo\b|FMV|tech-?demo|test suite|"
                      r"input test|system check|test patterns|monitor test", re.I)
APOSTA = re.compile(r"mahjong|mahjongg|majong|hanafuda|shougi|sugoroku|pachinko|"
                    r"pachi-slot|slot machine|poker|casino|bingo|keno", re.I)


def kind(g, name, desc, manuf, drv):
    if g.get("isbios") == "yes":
        return "bios"
    genres, marks = drv[0], drv[2]
    if name not in KEEP:
        both = desc + " " + manuf
        # marcas do driver; a descrição cobre o que o driver não marca
        if "homebrew" in marks or "demo" in marks or HOMEBREW.search(both) or "@" in manuf:
            return "homebrew"
        if "hack" in marks or re.search(r"\bhack\b", both, re.I) or \
                re.search(r"conversion\)", desc, re.I):
            return "hack"
        if "bootleg" in marks or re.search(r"\bbootleg\b", both, re.I):
            return "bootleg"
    if {"MAHJONG", "CASINO"} & set(genres) or APOSTA.search(desc):
        return "aposta"
    if "QUIZ" in genres or re.search(r"\bquiz\b", desc, re.I):
        return "quiz"
    if "JAMMA PCB" in desc:
        return "pcb"
    if drv[3] and "funciona" not in marks:
        return "quebrado"
    return "jogo"


def clean(s):
    return (s or "").replace("\t", " ").replace("\n", " ").strip()


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    drivers = importlib.import_module("fbneo-drivers")
    rows_drv = {}
    for d, _, files in os.walk(os.path.join(sys.argv[2], "drv")):
        for fn in files:
            if fn.endswith(".cpp"):
                drivers.parse_file(os.path.join(d, fn), rows_drv)
    drv = {}
    for z, line in rows_drv.items():
        _, g, p, m = line.split("\t")
        drv[z] = (g.split(",") if g else [], p, m.split(",") if m else [], True)
    root = ET.parse(sys.argv[1]).getroot()
    rows = []
    for g in root.iter("game"):
        name = g.get("name")
        desc = clean(g.findtext("description")) or name
        year = clean(g.findtext("year"))
        manuf = clean(g.findtext("manufacturer"))
        parent = g.get("cloneof") or ""
        system = (g.get("sourcefile") or "").split("/")[0]
        d = drv.get(name, ([], "", [], False))
        rows.append("\t".join([name, desc, year, manuf, parent, system,
                               kind(g, name, desc, manuf, d), ",".join(d[0]), d[1],
                               "1" if "vertical" in d[2] else "0"]))
    rows.sort()
    with open(sys.argv[3], "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    print(f"{sys.argv[3]}: {len(rows)} jogos")


if __name__ == "__main__":
    main()
