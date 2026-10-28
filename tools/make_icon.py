#!/usr/bin/env python3
"""Generates the WarioBlast Recomp icon (original pixel art: a lit bomb) in
every size the project needs.

usage: tools/make_icon.py        (writes assets/icon/* and runtime/icon_data.h)

Outputs:
  assets/icon/warioblast-{16,32,48,64,128,256}.png
  assets/icon/warioblast.ico          Windows icon (all sizes, PNG-compressed)
  runtime/icon_data.h           64x64 RGBA for the window icon
The 16 px size is drawn separately; larger sizes are the 32 px art scaled
by whole pixels."""

import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAL = {
    ".": (0, 0, 0, 0),
    "K": (20, 16, 32, 255),     # outline
    "N": (92, 40, 120, 255),    # background
    "n": (120, 56, 150, 255),   # background, upper half
    "D": (44, 40, 60, 255),     # bomb
    "d": (26, 22, 38, 255),     # bomb shadow
    "H": (110, 104, 140, 255),  # bomb highlight
    "w": (220, 216, 240, 255),  # bomb glint
    "S": (160, 160, 176, 255),  # fuse cap
    "s": (90, 90, 104, 255),    # fuse cap shadow
    "F": (210, 170, 110, 255),  # fuse
    "Y": (255, 230, 80, 255),   # spark
    "O": (255, 140, 40, 255),   # spark, outer
}


class Canvas:
    def __init__(self, size):
        self.size = size
        self.px = [["."] * size for _ in range(size)]

    def put(self, x, y, c):
        if 0 <= x < self.size and 0 <= y < self.size:
            self.px[y][x] = c

    def rounded(self, x, y, inset, r):
        x0 = y0 = inset
        x1 = y1 = self.size - 1 - inset
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = x0 + r if x < x0 + r else (x1 - r if x > x1 - r else x)
        cy = y0 + r if y < y0 + r else (y1 - r if y > y1 - r else y)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r * 0.8

    def background(self, r):
        for y in range(self.size):
            for x in range(self.size):
                if self.rounded(x, y, 0, r):
                    self.put(x, y, "K")
                if self.rounded(x, y, 1, r - 1):
                    self.put(x, y, "n" if y < self.size * 7 // 16 else "N")

    def bomb(self, cx, cy, r):
        """Round bomb lit from the upper left, with a 1 px outline."""
        def inside(x, y):
            return (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r
        for y in range(self.size):
            for x in range(self.size):
                dx, dy = x + 0.5 - cx, y + 0.5 - cy
                if inside(x, y):
                    if (dx + r * 0.4) ** 2 + (dy + r * 0.4) ** 2 < (r * 0.3) ** 2:
                        self.put(x, y, "H")
                    elif dx + dy > r * 0.7:
                        self.put(x, y, "d")
                    else:
                        self.put(x, y, "D")
                elif any(inside(x + ex, y + ey) for ex, ey in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    self.put(x, y, "K")

    def sprite(self, x0, y0, rows):
        for j, row in enumerate(rows):
            for i, c in enumerate(row):
                if c != ".":
                    self.put(x0 + i, y0 + j, c)


def icon32():
    c = Canvas(32)
    c.background(6)
    c.bomb(14, 18.5, 9.2)
    c.put(9, 14, "w")
    c.put(10, 14, "w")
    c.put(9, 15, "w")
    c.sprite(18, 3, [           # fuse cap, fuse and spark
        "......O.O.",
        "..O....Y..",
        "...Y.OYYYO",
        "....YYFY..",
        "...O.F.O..",
        ".....F..O.",
        "...KKF....",
        "..KSSsK...",
        ".KSSSssK..",
        ".KSssssK..",
        "..KsssK...",
        "...KKK....",
    ])
    return c


def icon16():
    c = Canvas(16)
    c.background(3)
    c.bomb(7.5, 9.5, 4.3)
    c.put(5, 7, "w")
    c.sprite(8, 1, [
        "...O.O",
        "....Y.",
        "...F.O",
        "..KF..",
        ".KSsK.",
        ".KssK.",
        "..KK..",
    ])
    return c


def rgba(canvas, scale):
    n = canvas.size * scale
    return [[PAL[canvas.px[y // scale][x // scale]] for x in range(n)] for y in range(n)]


def png_bytes(img):
    h, w = len(img), len(img[0])
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in row) for row in img)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico_bytes(pngs):
    """ICO with PNG-compressed images (supported since Windows Vista)."""
    head = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    dirs, data = b"", b""
    for size, blob in pngs:
        dim = 0 if size >= 256 else size
        dirs += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    return head + dirs + data


def main():
    out = os.path.join(ROOT, "assets", "icon")
    os.makedirs(out, exist_ok=True)
    big, small = icon32(), icon16()
    images = {16: rgba(small, 1)}
    for size in (32, 48, 64, 128, 256):
        images[size] = rgba(big, size // 32) if size % 32 == 0 else None
    # 48 px: 16 px art x3 stays crisp where 32 px art would need 1.5x
    images[48] = rgba(small, 3)
    pngs = []
    for size, img in sorted(images.items()):
        blob = png_bytes(img)
        with open(os.path.join(out, f"warioblast-{size}.png"), "wb") as f:
            f.write(blob)
        pngs.append((size, blob))
    with open(os.path.join(out, "warioblast.ico"), "wb") as f:
        f.write(ico_bytes(pngs))

    img = images[64]
    flat = [v for row in img for p in row for v in p]
    lines = [", ".join(f"0x{v:02x}" for v in flat[i:i + 16]) for i in range(0, len(flat), 16)]
    with open(os.path.join(ROOT, "runtime", "icon_data.h"), "w") as f:
        f.write("/* generated by tools/make_icon.py - do not edit */\n"
                "#ifndef ICON_DATA_H\n#define ICON_DATA_H\n\n"
                "#define ICON_SIZE 64\n"
                "static const unsigned char icon_rgba[ICON_SIZE * ICON_SIZE * 4] = {\n    "
                + ",\n    ".join(lines) + "\n};\n\n#endif\n")
    print("wrote", ", ".join(f"warioblast-{s}.png" for s in sorted(images)), "warioblast.ico, runtime/icon_data.h")


if __name__ == "__main__":
    main()
