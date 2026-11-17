#!/usr/bin/env python3
"""Baixa as imagens da galeria do Pingo Arcade do libretro-thumbnails.

Uso: arcade-midia.py <arcade.tsv> <lista-de-zips> <saida>

<lista-de-zips>: um nome por linha (o "ls" de Roms/ no cartão). Para cada
jogo grava <saida>/<zip>/snap.png (tela do jogo), titulo.png (tela de
título) e capa.png (reduzida para 600 px de altura, se houver o ImageMagick).
Os nomes do acervo são a descrição do DAT do FBNeo com &*/:`<>?\\|" trocados
por _; um clone sem imagem usa a do jogo pai. O que já existe na saída não
é baixado de novo, então dá para rodar outra vez depois de pôr mais jogos.
"""
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

REPO = "libretro-thumbnails/FBNeo_-_Arcade_Games"
TREE = f"https://api.github.com/repos/{REPO}/git/trees/master?recursive=1"
RAW = f"https://raw.githubusercontent.com/{REPO}/master/"
KINDS = (("Named_Snaps", "snap.png"), ("Named_Titles", "titulo.png"), ("Named_Boxarts", "capa.png"))
UA = {"User-Agent": "pingo-arcade-midia"}


def sanitize(desc):
    return re.sub(r'[&*/:`<>?\\|"]', "_", desc)


def fetch(url, tries=4):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError) as e:
            if i == tries - 1 or (isinstance(e, urllib.error.HTTPError) and e.code == 404):
                raise
            time.sleep(2 * (i + 1))


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    tsv, lista, out = sys.argv[1:]

    games = {}
    with open(tsv, encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            games[c[0]] = (c[1], c[4])  # descrição, pai

    # a lista do acervo (a API do GitHub às vezes responde 500): guarda a
    # última que veio e usa ela se a consulta falhar
    cache = os.path.join(out, ".acervo.json")
    try:
        tree = fetch(TREE)
        os.makedirs(out, exist_ok=True)
        with open(cache, "wb") as f:
            f.write(tree)
    except Exception as e:
        if not os.path.exists(cache):
            raise
        print(f"lista do acervo: {e}; usando a guardada", file=sys.stderr)
        with open(cache, "rb") as f:
            tree = f.read()
    have = {}
    for e in json.loads(tree)["tree"]:
        if e["type"] == "blob" and "/" in e["path"] and e["path"].endswith(".png"):
            d, name = e["path"].split("/", 1)
            have.setdefault(d, set()).add(name[:-4])

    jobs = []
    sem = []
    with open(lista, encoding="utf-8") as f:
        zips = [os.path.splitext(l.strip())[0] for l in f if l.strip()]
    for z in zips:
        if z not in games:
            continue
        for d, dest in KINDS:
            path = os.path.join(out, z, dest)
            if os.path.exists(path):
                continue
            for k in (z, games[z][1]):
                if k in games and sanitize(games[k][0]) in have.get(d, ()):
                    jobs.append((d + "/" + sanitize(games[k][0]) + ".png", path))
                    break
            else:
                if dest == "snap.png":
                    sem.append(z)

    magick = shutil.which("magick")

    def get(job):
        src, path = job
        data = fetch(RAW + urllib.parse.quote(src))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        if path.endswith("capa.png") and magick:
            subprocess.run([magick, tmp, "-resize", "x600>", "-strip", "png:" + tmp], check=True)
        os.replace(tmp, path)

    print(f"{len(zips)} zips, {len(jobs)} imagens para baixar, {len(sem)} sem tela de jogo")
    erros = 0
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for i, r in enumerate(ex.map(lambda j: _try(get, j), jobs), 1):
            if r:
                erros += 1
                print(r, file=sys.stderr)
            if i % 500 == 0:
                print(f"  {i}/{len(jobs)}")
    print(f"pronto: {len(jobs) - erros} baixadas, {erros} erros")
    if sem:
        print("sem tela de jogo:", " ".join(sem))


def _try(fn, job):
    try:
        fn(job)
    except Exception as e:  # segue com os outros; rodar de novo completa
        return f"{job[0]}: {e}"
    return None


if __name__ == "__main__":
    main()
