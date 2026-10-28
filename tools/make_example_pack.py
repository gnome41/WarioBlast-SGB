#!/usr/bin/env python3
"""Builds the example mod pack in examples/mods/WarioBlast-Example from scratch.

usage: make_example_pack.py [OUT_DIR]

Everything in the pack is drawn or composed by this script - no game
graphics or audio are used - so the pack can be shared:

  tiles/   HD versions of the status-bar digits 0-9 (stage timer, enemy
           count; color = indexed, so the game's own palettes and Super
           Game Boy colours apply)
  border/  an HD Super Game Boy border: a bomb arena with stone pillars,
           soft blocks, lit bombs and a blast
  audio/   a new chiptune for the first stage (music 0B), with an intro
           and a loop point in music_0b.ini

The only things taken from the game are the tile names: each is the 16
bytes of the original tile, which is how the runtime finds the tile to
replace (see MODDING.md).  Python 3 standard library only; if ffmpeg with
libvorbis is installed the music is written as OGG, otherwise as WAV."""

import math
import os
import shutil
import struct
import subprocess
import sys
import zlib

S = 4   # pack scale: tiles are 32x32, the border 1024x896


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Rng:
    """Small deterministic generator, so the pack is the same on every run."""
    def __init__(self, seed):
        self.s = seed

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return self.s / 0x7FFFFFFF


# ---- shape helpers (signed distances, in pixels) ---------------------------

def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy or 1)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def arc(cx, cy, rx, ry, a0, a1, n=24):
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def poly_dist(px, py, strokes):
    d = 1e9
    for pts in strokes:
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            d = min(d, seg_dist(px, py, ax, ay, bx, by))
    return d


# ---- tiles: timer digits ----------------------------------------------------

# The original 8x8 tiles (2 bits per pixel, 16 bytes) of the digits 0-9 the
# game shows in its status bar; the file names must be these.
DIGIT_KEYS = [
    "00007c7cc6c6c6c6c6c6c6c67c7c0000", "0000383878783838383838387c7c0000",
    "00007c7ccece0e0e38387070fefe0000", "00007c7ccece1c1c0e0ecece7c7c0000",
    "00001c1c3c3c5c5c9c9cfefe1c1c0000", "0000fcfce0e0fcfc0e0ecece7c7c0000",
    "00007c7ce0e0fcfce6e6e6e67c7c0000", "0000fefe0e0e1c1c3838383838380000",
    "00007c7ce6e67c7ce6e6e6e67c7c0000", "00007c7ccececece7e7e0e0e7c7c0000",
]

# New glyphs as strokes in a 32x32 tile (rounded, bold, drawn for this pack).
DIGIT_STROKES = [
    [arc(15.5, 15, 6.5, 9.5, 0, 360, 40)],
    [[(15.5, 5.5), (15.5, 24.5)], [(15.5, 5.5), (11, 9.5)], [(11.5, 24.5), (19.5, 24.5)]],
    [arc(15.5, 10.5, 6, 5, 195, 380) + [(9.5, 24.5), (21.5, 24.5)]],
    [arc(15.5, 10, 5.5, 4.6, 200, 450), arc(15.5, 19.6, 6.2, 5.2, 270, 520)],
    [[(18.5, 24.5), (18.5, 5.5), (9.2, 18.5), (22.5, 18.5)]],
    [[(21, 5.5), (11, 5.5), (10.2, 14.2)] + arc(15.5, 18.6, 6.2, 6, 232, 500)],
    [arc(16.5, 15.5, 6.8, 10, 295, 180) + [(9.7, 19)], arc(15.6, 19, 6, 5.7, 0, 360, 40)],
    [[(9.5, 5.5), (21.5, 5.5), (13, 24.5)]],
    [arc(15.5, 10, 5.2, 4.6, 0, 360, 36), arc(15.5, 19.6, 6.2, 5.2, 0, 360, 40)],
    [arc(15.4, 11, 6, 5.7, 0, 360, 40), [(21.4, 11), (21.4, 14)] + arc(14.6, 14, 6.8, 10, 0, 115)],
]


def digit_tile(strokes):
    """Colour indices (as greys, see MODDING.md): body 3, shadow 1,
    bevel 2, background 0 - the original digits are colour 3 on 0, so
    this is a black digit with a light grey shadow and a dark grey bevel."""
    grey = [255, 170, 85, 0]
    px = bytearray(32 * 32 * 4)
    for y in range(32):
        for x in range(32):
            cx, cy = x + 0.5, y + 0.5
            idx = 0
            if poly_dist(cx - 2.2, cy - 2.2, strokes) < 2.9:
                idx = 1                                        # drop shadow
            if poly_dist(cx, cy, strokes) < 2.9:
                idx = 3                                        # body
                if poly_dist(cx + 1.0, cy + 1.0, strokes) < 0.9:
                    idx = 2                                    # bevel
            v = grey[idx]
            px[(y * 32 + x) * 4:(y * 32 + x) * 4 + 4] = bytes((v, v, v, 255))
    return px


# ---- border -----------------------------------------------------------------

W, H = 256 * S, 224 * S
GAME = (48, 40, 208, 184)       # the game screen inside the border, native pixels


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def blend(self, x, y, col, a):
        if 0 <= x < self.w and 0 <= y < self.h and a > 0:
            p = self.px[y * self.w + x]
            a = min(1.0, a) * (col[3] / 255 if len(col) > 3 else 1.0)
            for i in range(3):
                p[i] = p[i] * (1 - a) + col[i] * a
            p[3] = p[3] * (1 - a) + a

    def shape(self, box, dist, col):
        """Fills where dist(x, y) < 0 (native units), anti-aliased."""
        x0, y0, x1, y1 = (int(v * S) for v in box)
        for y in range(max(0, y0 - 1), min(self.h, y1 + 2)):
            for x in range(max(0, x0 - 1), min(self.w, x1 + 2)):
                d = dist((x + 0.5) / S, (y + 0.5) / S) * S
                self.blend(x, y, col, 0.5 - d)

    def circle(self, cx, cy, r, col):
        self.shape((cx - r, cy - r, cx + r, cy + r), lambda x, y: math.hypot(x - cx, y - cy) - r, col)

    def rrect(self, x0, y0, x1, y1, r, col):
        cx, cy, hw, hh = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2

        def d(x, y):
            qx, qy = abs(x - cx) - hw + r, abs(y - cy) - hh + r
            return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
        self.shape((x0, y0, x1, y1), d, col)

    def line(self, ax, ay, bx, by, r, col):
        self.shape((min(ax, bx) - r, min(ay, by) - r, max(ax, bx) + r, max(ay, by) + r),
                   lambda x, y: seg_dist(x, y, ax, ay, bx, by) - r, col)

    def rgba(self):
        out = bytearray(self.w * self.h * 4)
        for i, (r, g, b, a) in enumerate(self.px):
            out[i * 4:i * 4 + 4] = bytes((int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5)))
        return out


def pillar(c, x, y):
    """An indestructible 16x16 stone block."""
    c.rrect(x, y, x + 16, y + 16, 1.5, (52, 48, 72))
    c.rrect(x + 1, y + 1, x + 15, y + 15, 1.2, (132, 128, 160))
    c.rrect(x + 3, y + 3, x + 15, y + 15, 1.0, (86, 82, 112))
    c.rrect(x + 3, y + 3, x + 13, y + 13, 0.8, (110, 106, 140))
    c.circle(x + 3.2, y + 3.2, 0.8, (210, 206, 230))


def soft_block(c, x, y):
    """A breakable 16x16 brick block."""
    c.rrect(x, y, x + 16, y + 16, 1.0, (92, 40, 26))
    for row in range(4):
        off = 0 if row % 2 else 4
        for k in range(-1, 3):
            bx = x + off + k * 8
            x0, x1 = max(x + 1, bx + 0.6), min(x + 15, bx + 7.4)
            if x1 - x0 > 1:
                c.rrect(x0, y + 1 + row * 3.6, x1, y + 3.9 + row * 3.6, 0.6, (196, 104, 58))
                c.rrect(x0, y + 1 + row * 3.6, x1, y + 1.8 + row * 3.6, 0.3, (236, 160, 104))


def bomb(c, cx, cy, r):
    """A lit bomb: round body, cap, fuse and spark."""
    c.circle(cx + 1, cy + 1.5, r, (0, 0, 0, 90))
    c.circle(cx, cy, r + 0.8, (12, 10, 20))
    c.circle(cx, cy, r, (40, 38, 58))
    c.circle(cx - r * 0.35, cy - r * 0.35, r * 0.32, (112, 108, 146))
    c.circle(cx - r * 0.45, cy - r * 0.45, r * 0.12, (230, 226, 250))
    t = math.radians(-50)
    kx, ky = cx + math.cos(t) * r * 0.95, cy + math.sin(t) * r * 0.95
    c.rrect(kx - 2, ky - 2, kx + 2, ky + 2, 0.8, (150, 150, 170))
    fx, fy = kx + 2.6, ky - 3.4
    c.line(kx + 0.5, ky - 1.5, fx, fy, 0.55, (214, 176, 116))
    for a, l in ((0, 2.8), (72, 2.2), (144, 2.8), (216, 2.2), (288, 2.8)):
        t = math.radians(a + 15)
        c.line(fx, fy, fx + math.cos(t) * l, fy + math.sin(t) * l, 0.45, (255, 150, 40))
    c.circle(fx, fy, 1.3, (255, 236, 110))


def blast(c, cx, cy, arm):
    """A cross-shaped explosion centred on (cx, cy), arms `arm` long."""
    for col, w in (((200, 40, 20), 6.5), ((255, 140, 30), 5.0), ((255, 222, 90), 3.2), ((255, 255, 230), 1.6)):
        c.rrect(cx - arm, cy - w, cx + arm, cy + w, w, col)
        c.rrect(cx - w, cy - arm, cx + w, cy + arm, w, col)
        c.circle(cx, cy, w * 1.5, col)


# 5x7 letters for the caption (drawn for this pack)
FONT = {
    "D": "11110 10001 10001 10001 10001 10001 11110", "K": "10001 10010 10100 11000 10100 10010 10001",
    "9": "01110 10001 10001 01111 00001 00010 01100", "4": "00010 00110 01010 10010 11111 00010 00010",
    "R": "11110 10001 10001 11110 10100 10010 10001", "E": "11111 10000 10000 11110 10000 10000 11111",
    "C": "01110 10001 10000 10000 10000 10001 01110", "O": "01110 10001 10001 10001 10001 10001 01110",
    "M": "10001 11011 10101 10101 10001 10001 10001", "P": "11110 10001 10001 11110 10000 10000 10000",
    "X": "10001 10001 01010 00100 01010 10001 10001", "A": "01110 10001 10001 11111 10001 10001 10001",
    "L": "10000 10000 10000 10000 10000 10000 11111", " ": "00000 00000 00000 00000 00000 00000 00000",
    "W": "10001 10001 10001 10101 10101 10101 01010", "I": "01110 00100 00100 00100 00100 00100 01110",
    "B": "11110 10001 10001 11110 10001 10001 11110", "S": "01111 10000 10000 01110 00001 00001 11110",
    "T": "11111 00100 00100 00100 00100 00100 00100", "U": "10001 10001 10001 10001 10001 10001 01110",
    "N": "10001 11001 10101 10011 10001 10001 10001", "G": "01110 10001 10000 10111 10001 10001 01111",
}


def text(c, s, cx, y, col, shadow):
    w = len(s) * 6 - 1
    x0 = cx - w / 2
    for i, ch in enumerate(s):
        for ry, row in enumerate(FONT[ch].split()):
            for rx, bit in enumerate(row):
                if bit == "1":
                    px, py = x0 + i * 6 + rx, y + ry
                    c.rrect(px + 0.5, py + 0.5, px + 1.5, py + 1.5, 0.25, shadow)
                    c.rrect(px, py, px + 1, py + 1, 0.25, col)


def border():
    c = Canvas(W, H)
    rnd = Rng(1994)
    # arena floor: a checkerboard of 16x16 stone slabs with a little grain
    for y in range(H):
        for x in range(W):
            tx, ty = x // (16 * S), y // (16 * S)
            col = (70, 112, 70) if (tx + ty) % 2 else (82, 128, 80)
            c.px[y * W + x] = [col[0], col[1], col[2], 1.0]
    for _ in range(500):
        x, y = rnd.next() * 256, rnd.next() * 224
        c.circle(x, y, 0.3 + 0.4 * rnd.next(), (40, 70, 44, 120))
    # stone pillars round the outside, soft blocks scattered between them
    gx0, gy0, gx1, gy1 = GAME
    for ty in range(14):
        for tx in range(16):
            x, y = tx * 16, ty * 16
            if gx0 - 8 < x + 16 and x < gx1 + 8 and gy0 - 8 < y + 16 and y < gy1 + 8:
                continue      # keep clear of the screen and its bezel
            if tx in (0, 15) or ty in (0, 13) or (tx % 2 == 0 and ty % 2 == 0):
                pillar(c, x, y)
            elif rnd.next() < 0.45:
                soft_block(c, x, y)
    # a blast between two pillars, and a few bombs about to go off
    blast(c, 24, 104, 14)
    for bx, by in ((40, 24), (216, 56), (232, 168), (24, 200)):
        bomb(c, bx, by, 5)
    # screen bezel
    c.rrect(gx0 - 6, gy0 - 6, gx1 + 6, gy1 + 6, 5, (10, 10, 18))
    c.rrect(gx0 - 4.5, gy0 - 4.5, gx1 + 4.5, gy1 + 4.5, 3.5, (120, 74, 150))
    c.rrect(gx0 - 3, gy0 - 3, gx1 + 3, gy1 + 3, 2.5, (26, 26, 38))
    for sx, sy in ((gx0 - 3.6, gy0 - 3.6), (gx1 + 3.6, gy0 - 3.6), (gx0 - 3.6, gy1 + 3.6), (gx1 + 3.6, gy1 + 3.6)):
        c.circle(sx, sy, 1.0, (230, 200, 250))
    # caption plate
    c.rrect(70, 197, 186, 222, 3, (20, 14, 34, 220))
    text(c, "WARIO BLAST RECOMP", 128, 200, (255, 222, 90), (90, 30, 30))
    text(c, "EXAMPLE PACK", 128, 212, (190, 150, 250), (30, 20, 60))
    # the game screen must be fully transparent
    for y in range(gy0 * S, gy1 * S):
        for x in range(gx0 * S, gx1 * S):
            c.px[y * W + x] = [0.0, 0.0, 0.0, 0.0]
    return c.rgba()


# ---- music ------------------------------------------------------------------

RATE = 32000
STEP = 60 / 150 / 4             # a 16th note at 150 bpm

NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(n):
    return 12 * (int(n[-1]) + 1) + NOTE[n[0]] + (1 if "#" in n else 0)


# lead: note/16ths, "-" = rest.  Bars 1-8: verse, 9-16: bridge.
LEAD = """
E5/2 G5/2 C6/3 G5/1 E5/2 G5/2 A5/4 | A5/2 G5/2 E5/2 C5/2 D5/2 E5/2 A4/4 |
F5/2 A5/2 C6/2 A5/2 G5/2 F5/2 E5/2 D5/2 | D5/3 E5/1 F5/2 G5/2 B5/4 -/2 G5/2 |
E5/2 G5/2 C6/3 G5/1 E6/4 D6/2 C6/2 | A5/2 C6/2 B5/2 A5/2 G5/2 E5/2 C5/4 |
F5/2 E5/2 F5/2 A5/2 G5/4 F5/2 D5/2 | G5/4 F5/2 E5/2 D5/4 B4/4 |
A5/4 C6/2 A5/2 F5/4 A5/4 | B5/4 D6/2 B5/2 G5/4 B5/4 |
E6/3 D6/1 B5/2 G5/2 E5/4 G5/4 | A5/2 B5/2 C6/2 D6/2 E6/8 |
F6/2 E6/2 C6/2 A5/2 D6/2 C6/2 A5/4 | G5/2 A5/2 B5/2 D6/2 C6/2 B5/2 G5/4 |
C6/4 G5/2 E5/2 G5/2 C6/2 E6/4 | D6/2 C6/2 B5/2 G5/2 C6/8
"""
CHORDS = "C Am F G C Am F G F G Em Am F G C C".split()
CHORD_NOTES = {"C": (0, 4, 7), "Am": (9, 12, 16), "F": (5, 9, 12), "G": (7, 11, 14), "Em": (4, 7, 11)}


def square(phase, duty):
    return 1.0 if (phase % 1.0) < duty else -1.0


def triangle(phase):
    p = phase % 1.0
    return 4 * p - 1 if p < 0.5 else 3 - 4 * p


def music():
    bars = len(CHORDS) + 1                  # + 1 bar intro
    n = int(bars * 16 * STEP * RATE)
    mix = [0.0] * n
    intro = int(16 * STEP * RATE)

    def tone(start, dur, freq, wave, vol, vib=0.0):
        s0, s1 = int(start * RATE), min(n, int((start + dur) * RATE))
        ph = 0.0
        for i in range(s0, s1):
            t = (i - s0) / RATE
            env = min(1.0, t / 0.004) * (0.75 + 0.25 * math.exp(-t * 12)) * min(1.0, (s1 - i) / (RATE * 0.012))
            f = freq * (1 + (vib * math.sin(2 * math.pi * 5.5 * t) if t > 0.15 else 0))
            ph += f / RATE
            mix[i] += wave(ph) * vol * env

    def hz(m):
        return 440 * 2 ** ((m - 69) / 12)

    t = 16 * STEP                            # lead starts after the intro
    for tok in LEAD.replace("|", " ").split():
        note, d = tok.split("/")
        d = int(d) * STEP
        if note != "-":
            tone(t, d * 0.92, hz(midi(note) - 12), lambda p: square(p, 0.25), 0.30, vib=0.006)
        t += d
    for b, ch in enumerate(CHORDS):
        t0 = (b + 1) * 16 * STEP
        root = 36 + CHORD_NOTES[ch][0] % 12
        for k, off in enumerate((0, 12, 7, 12, 0, 12, 7, 12)):          # bass in 8ths
            tone(t0 + k * 2 * STEP, 2 * STEP * 0.8, hz(root + off), triangle, 0.42)
        for k in range(16):                                              # arpeggio
            m = 60 + CHORD_NOTES[ch][k % 3]
            tone(t0 + k * STEP, STEP * 0.7, hz(m), lambda p: square(p, 0.125), 0.07)

    lfsr = 0x7FFF

    def noise():
        nonlocal lfsr
        bit = (lfsr ^ (lfsr >> 1)) & 1
        lfsr = (lfsr >> 1) | (bit << 14)
        return 1.0 if lfsr & 1 else -1.0

    def drum(start, kind):
        s0 = int(start * RATE)
        length = {"k": 0.14, "s": 0.12, "h": 0.03}[kind]
        ph = 0.0
        held = 0.0
        for i in range(s0, min(n, s0 + int(length * RATE))):
            t = (i - s0) / RATE
            if kind == "k":
                ph += (55 + 120 * math.exp(-t * 40)) / RATE
                v = math.sin(2 * math.pi * ph) * 0.55 * math.exp(-t * 22)
            else:
                if (i - s0) % (1 if kind == "h" else 2) == 0:
                    held = noise()
                v = held * (0.22 if kind == "s" else 0.10) * math.exp(-t * (30 if kind == "s" else 120))
            mix[i] += v

    for k in (0, 4, 8):                                                  # intro
        drum(k * STEP, "k")
    for k in (12, 13, 14, 15):
        drum(k * STEP, "s")
    for b in range(1, bars):
        t0 = b * 16 * STEP
        for k in range(16):
            if k in (0, 8, 10):
                drum(t0 + k * STEP, "k")
            if k in (4, 12):
                drum(t0 + k * STEP, "s")
            if k % 2 == 0:
                drum(t0 + k * STEP, "h")

    lp, out = 0.0, bytearray()
    peak = max(abs(v) for v in mix) or 1.0
    for v in mix:
        lp += (v - lp) * 0.45                                            # soften the square waves
        out += struct.pack("<h", int(lp / peak * 13000))
    return out, intro


def write_wav(path, pcm):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) +
                b"data" + struct.pack("<I", len(pcm)) + pcm)


# ---- pack -------------------------------------------------------------------

PACK_INI = """\
; Example pack for WarioBlast Recomp - everything in it is original work, made by
; tools/make_example_pack.py (MIT licence, like the rest of the project).
name = WarioBlast Example Pack
author = WarioBlast Recomp project
version = 1.0
description = HD border, HD status-bar digits and a new first-stage tune
scale = {scale}
; indexed: the digits are drawn with the four greys, so the game's palettes
; and Super Game Boy colours apply to them (the border is always full colour)
color = indexed
"""


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "examples", "mods", "WarioBlast-Example")
    for sub in ("tiles", "border", "audio"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    with open(os.path.join(out, "pack.ini"), "w") as f:
        f.write(PACK_INI.format(scale=S))

    for key, strokes in zip(DIGIT_KEYS, DIGIT_STROKES):
        write_png(os.path.join(out, "tiles", key + ".png"), 32, 32, digit_tile(strokes))
    write_png(os.path.join(out, "border", "border.png"), W, H, border())

    pcm, intro = music()
    adir = os.path.join(out, "audio")
    for old in ("music_0b.wav", "music_0b.ogg"):
        if os.path.exists(os.path.join(adir, old)):
            os.remove(os.path.join(adir, old))
    wav = os.path.join(adir, "music_0b.wav")
    write_wav(wav, pcm)
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg and subprocess.run([ffmpeg, "-loglevel", "error", "-y", "-i", wav, "-c:a", "libvorbis",
                                  "-q:a", "5", "-map_metadata", "-1", "-fflags", "+bitexact",
                                  os.path.join(adir, "music_0b.ogg")]).returncode == 0:
        os.remove(wav)
    with open(os.path.join(adir, "music_0b.ini"), "w") as f:
        f.write("; the first bar is an intro; the song then loops from bar 2 to the end\n"
                f"loop_start = {intro}\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
