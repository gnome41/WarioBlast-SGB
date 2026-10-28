#!/usr/bin/env python3
"""Test harness for the recompiled build.

  verify.py compare  [--exe build/warioblast] [--seeds 1-8] [--frames 20000]
      Runs every seed twice - on the interpreter (--interp) and with the
      recompiled code - and checks the per-frame state hashes are identical.
      --view wide|frame runs both with that display view (render-only, so
      the hashes must also equal those of the original view).

  verify.py coverage [--exe build/warioblast] [--seeds 1-32] [--frames 30000]
      Runs the interpreter with pseudo-random input and merges every ROM
      address it executed into tools/recomp/coverage.txt, so the next build
      recompiles that code too.

  verify.py fallback [--exe build/warioblast] [--seeds 1-8] [--frames 20000]
      Reports how much code still runs on the interpreter in the
      recompiled build.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COVERAGE = os.path.join(ROOT, "tools", "recomp", "coverage.txt")


def seeds(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out


VIEW = []


def run(exe, seed, frames, extra, env=None):
    cmd = [exe, "--headless", "--unthrottled", "--no-save", "--frames", str(frames),
           "--input", f"random:{seed}"] + VIEW + extra
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({r.returncode}):\n{r.stderr}")
    return r


def cmd_compare(a, tmp):
    def one(seed):
        hi = os.path.join(tmp, f"i{seed}.txt")
        hr = os.path.join(tmp, f"r{seed}.txt")
        run(a.exe, seed, a.frames, ["--interp", "--hash-log", hi])
        run(a.exe, seed, a.frames, ["--hash-log", hr])
        li, lr = open(hi).read().splitlines(), open(hr).read().splitlines()
        for x, y in zip(li, lr):
            if x != y:
                return seed, f"DIVERGES at frame {x.split()[0]}"
        if len(li) != len(lr):
            return seed, f"length mismatch {len(li)} vs {len(lr)}"
        return seed, f"identical ({len(li)} frames)"

    ok = True
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        for seed, msg in ex.map(one, seeds(a.seeds)):
            print(f"seed {seed}: {msg}")
            ok &= msg.startswith("identical")
    return 0 if ok else 1


def read_refs(path):
    if not os.path.exists(path):
        return set()
    with open(path) as f:
        return {l.strip() for l in f if l.strip() and not l.startswith("#")}


def cmd_coverage(a, tmp):
    def one(seed):
        log = os.path.join(tmp, f"c{seed}.txt")
        run(a.exe, seed, a.frames, ["--interp", "--log-fallback", log])
        return read_refs(log)

    before = read_refs(COVERAGE)
    refs = set(before)
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        for r in ex.map(one, seeds(a.seeds)):
            refs |= r
    with open(COVERAGE, "w") as f:
        for r in sorted(refs):
            f.write(r + "\n")
    print(f"coverage: {len(before)} -> {len(refs)} addresses ({len(refs) - len(before)} new)")
    return 0


def cmd_fallback(a, tmp):
    env = dict(os.environ, WB_DEBUG="1")

    def one(seed):
        log = os.path.join(tmp, f"f{seed}.txt")
        r = run(a.exe, seed, a.frames, ["--log-fallback", log], env)
        n = 0
        for line in r.stderr.splitlines():
            if line.startswith("interpreted instructions:"):
                n = int(line.split(":")[1])
        return seed, n, read_refs(log)

    allrefs = set()
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        for seed, n, refs in ex.map(one, seeds(a.seeds)):
            print(f"seed {seed}: {n} instructions interpreted, {len(refs)} distinct ROM addresses")
            allrefs |= refs
    for r in sorted(allrefs)[:40]:
        print("  ", r)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["compare", "coverage", "fallback"])
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "warioblast"))
    ap.add_argument("--seeds", default=None)
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--view", choices=["original", "frame", "wide"],
                    help="display view to run with (must not change any hash)")
    a = ap.parse_args()
    if a.view:
        VIEW.extend(["--view", a.view])
    if a.seeds is None:
        a.seeds = "1-32" if a.command == "coverage" else "1-8"
    with tempfile.TemporaryDirectory() as tmp:
        return {"compare": cmd_compare, "coverage": cmd_coverage,
                "fallback": cmd_fallback}[a.command](a, tmp)


if __name__ == "__main__":
    sys.exit(main())
