#!/usr/bin/env python3
"""Builds a synthetic mod pack from a dump, for testing (and as an example
of the pack format).  The output contains the dumped game graphics
upscaled - keep it local, do not distribute it.

usage: make_test_pack.py DUMP_DIR OUT_PACK_DIR [--indexed] [--scale N]
                         [--music ID ...] [--sfx ID ...] [--name NAME]

Tiles are upscaled with a visible pattern (magenta stripe for rgba, a
dither of neighbouring colour indices for indexed), the border gets a grid,
and music/sfx ids get sine-tone WAVs."""

import argparse
import math
import os
import struct
import zlib


def read_png(path):
    d = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
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
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    px = [[tuple(r[x * bpp:x * bpp + bpp]) + ((255,) if bpp == 3 else ()) for x in range(w)] for r in rows]
    return w, h, px


def write_png(path, px):
    h, w = len(px), len(px[0])
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in row) for row in px)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def write_wav(path, freq, seconds, rate=22050):
    n = int(seconds * rate)
    data = b"".join(struct.pack("<hh", s, s) for s in
                    (int(9000 * math.sin(2 * math.pi * freq * i / rate)) for i in range(n)))
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16) + b"data" + struct.pack("<I", len(data))
    open(path, "wb").write(hdr + data)


GREY_TO_IDX = {255: 0, 170: 1, 85: 2, 0: 3}
IDX_TO_GREY = [255, 170, 85, 0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("out")
    ap.add_argument("--indexed", action="store_true")
    ap.add_argument("--exact", action="store_true",
                    help="indexed art that is exactly the original tile upscaled (output must equal native)")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--music", nargs="*", default=[])
    ap.add_argument("--sfx", nargs="*", default=[])
    ap.add_argument("--name", default="Test pack")
    a = ap.parse_args()
    S = a.scale
    if a.exact:
        a.indexed = True
    for sub in ("tiles", "border", "audio"):
        os.makedirs(os.path.join(a.out, sub), exist_ok=True)

    ref = {"ref_bgp": "E4", "ref_obp0": "E4", "ref_obp1": "E4"}
    tmpl = os.path.join(a.dump, "pack.ini")
    if os.path.exists(tmpl):
        for line in open(tmpl):
            k, _, v = line.partition("=")
            if k.strip() in ref and v.strip():
                ref[k.strip()] = v.strip()
    with open(os.path.join(a.out, "pack.ini"), "w") as f:
        f.write(f"name = {a.name}\nauthor = make_test_pack.py\nversion = 1.0\n"
                f"description = synthetic test pack\nscale = {S}\n"
                f"color = {'indexed' if a.indexed else 'rgba'}\n" +
                "".join(f"{k} = {v}\n" for k, v in ref.items()))

    tiles = sorted(os.listdir(os.path.join(a.dump, "tiles")))
    for name in tiles:
        _, _, px = read_png(os.path.join(a.dump, "tiles", name))
        out = []
        for y in range(8 * S):
            row = []
            for x in range(8 * S):
                r, g, b, al = px[y // S][x // S]
                idx = GREY_TO_IDX.get(r, 0) if al else 0
                if a.indexed:
                    # dither towards the next index on a checkerboard: visibly HD
                    if not a.exact and (x + y) % 2 and idx < 3 and (x % S) and (y % S):
                        idx += 1
                    v = IDX_TO_GREY[idx]
                    row.append((v, v, v, al))
                else:
                    shade = 255 - idx * 70
                    col = (shade, shade, shade)
                    if (x - y) % (S * 2) == 0 and al:
                        col = (255, 0, 200)        # magenta stripe: replaced
                    row.append(col + (al,))
            out.append(row)
        write_png(os.path.join(a.out, "tiles", name), out)

    bpath = os.path.join(a.dump, "border", "border.png")
    if os.path.exists(bpath) and not a.indexed:
        w, h, px = read_png(bpath)
        out = []
        for y in range(h * S):
            row = []
            for x in range(w * S):
                p = px[y // S][x // S]
                if p[3] and (x % (8 * S) == 0 or y % (8 * S) == 0):
                    p = (0, 255, 255, 255)         # cyan grid: HD border
                row.append(p)
            out.append(row)
        write_png(os.path.join(a.out, "border", "border.png"), out)

    for i, mid in enumerate(a.music):
        write_wav(os.path.join(a.out, "audio", f"music_{mid}.wav"), 220 * (i + 2), 2.0)
    for i, sid in enumerate(a.sfx):
        write_wav(os.path.join(a.out, "audio", f"sfx_{sid}.wav"), 1500 + 300 * i, 0.15)
    print(f"wrote {a.out}: {len(tiles)} tiles at {S}x, "
          f"{'indexed' if a.indexed else 'rgba'}, {len(a.music)} music, {len(a.sfx)} sfx")


if __name__ == "__main__":
    main()
