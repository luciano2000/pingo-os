#!/usr/bin/env python3
"""Converte um PNG (8 bits, RGB/RGBA, sem entrelaçamento) para o formato bruto
do framebuffer: XRGB8888 little-endian (bytes B, G, R, 0), alpha sobre preto.

Uso: png2fb.py entrada.png saida.raw

Só biblioteca padrão: o build roda num contêiner sem Pillow.
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: não é PNG")
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
        pos += 12 + length
    width, height, depth, color, _, _, interlace = ihdr
    if depth != 8 or color not in (2, 6) or interlace:
        sys.exit(f"{path}: só PNG de 8 bits RGB/RGBA sem entrelaçamento")
    bpp = 4 if color == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        rows.append(line)
        prev = line
    return width, height, bpp, rows


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    width, height, bpp, rows = read_png(sys.argv[1])
    out = bytearray()
    for line in rows:
        for x in range(width):
            r, g, b = line[x * bpp:x * bpp + 3]
            if bpp == 4:
                alpha = line[x * bpp + 3]
                r, g, b = (r * alpha // 255, g * alpha // 255, b * alpha // 255)
            out += bytes((b, g, r, 0))
    open(sys.argv[2], "wb").write(out)
    print(f"{sys.argv[2]}: {width}x{height} XRGB8888")


if __name__ == "__main__":
    main()
