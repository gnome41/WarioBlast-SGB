#!/usr/bin/env python3
"""Pre-release check: makes sure no game data ended up in release files.

usage: check_release.py ROM FILE...

Fails if any file contains a 32-byte run copied from the ROM (runs that
occur by coincidence - padding, fills, counting sequences - are ignored) or
names of recompiled functions (f_BB_AAAA), and requires the main executable
to be a player build - a developer build packaged by mistake would contain
the game's code."""

import os
import re
import sys

WINDOW = 32
PLAYER_MARKER = b"struct layout probe, compared with the host compiler"


def interesting(chunk):
    """Ignore runs that occur by coincidence: few distinct values (padding,
    fill) or a constant step in every byte lane of 8, 16 or 32-bit values
    (counting sequences, identity lookup tables)."""
    if len(set(chunk)) <= 4:
        return False
    for stride in (1, 2, 4):
        steps = [{(chunk[i + stride] - chunk[i]) & 0xFF for i in range(lane, len(chunk) - stride, stride)}
                 for lane in range(stride)]
        if all(len(s) == 1 for s in steps):
            return False
    return True


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rom = open(sys.argv[1], "rb").read()
    index = set()
    for i in range(0, len(rom) - WINDOW + 1):
        c = rom[i:i + WINDOW]
        if interesting(c):
            index.add(c)
    bad = False
    for path in sys.argv[2:]:
        data = open(path, "rb").read()
        hits = [i for i in range(0, len(data) - WINDOW + 1)
                if data[i:i + WINDOW] in index and interesting(data[i:i + WINDOW])]
        names = sorted(set(re.findall(rb"f_[0-9A-F]{2}_[0-9A-F]{4}", data)))
        # the main executable must be a player build (it carries the TinyCC
        # prelude); a developer build has the game code compiled in
        not_player = os.path.basename(path) in ("warioblast", "warioblast.exe") and PLAYER_MARKER not in data
        if hits or names or not_player:
            bad = True
            print(f"FAIL {path}: {len(hits)} ROM-matching runs, {len(names)} recompiled-function names"
                  + (", not a player build" if not_player else ""))
            for i in hits[:5]:
                print(f"     at {i:#x}: {data[i:i + WINDOW].hex()}")
        else:
            print(f"ok   {path}: no ROM data ({len(data)} bytes scanned)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
