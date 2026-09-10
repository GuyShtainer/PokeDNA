#!/usr/bin/env python3
"""Game Boy screen cell-diff oracle (BACKLOG #97; originated as the U4
review's own docs/shots/rvu4/celldiff.py).

Diffs the SHIPPED painter's cells (resolved to absolute Game Boy tile ids by
a small host-compiled harness that runs the real, unmodified C painter
functions -- see README.md in this directory for how to build one) against a
LIVE VRAM dump of the real cartridge (a *_grid.json this directory's own
oracle.py writes). Unlike hand-retyping the expected layout in Python, the
"model" side here is the compiled output of the real C functions -- it can
never drift from what actually ships.

No ROM/save/pixel data ships in this file. Point --sav at your own dumps
(gba-toolkit/roms/gb by default, matching the layout docs/kb/pokemon/ and
roms.sh describe); the *_grid.json ground-truth files live in
docs/shots/<slice>/ per review (gitignored, not committed -- see that
directory for the ones a given review actually produced)."""
import argparse
import json
import subprocess
import sys
from pathlib import Path


def painter(harness: Path, sav: Path, top: int, sel: int):
    out = subprocess.run([str(harness), str(sav), str(top), str(sel)],
                          capture_output=True, text=True).stdout
    g = []
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("["):
            continue
        g.append([int(v, 16) for v in line.strip("[],").split(",")])
    return g


def load_real(path: Path):
    return json.load(open(path))["grid"]


def run(tag, harness, real_json, sav, top, sel, blink=None):
    real = load_real(real_json)
    mine = painter(harness, sav, top, sel)
    blink = blink or {}
    cats = {}
    for y in range(18):
        for x in range(20):
            r, m = real[y][x], mine[y][x]
            alts = blink.get((y, x), set())
            if r == m or m in alts:
                continue
            inbox = (2 <= y <= 12 and 4 <= x <= 19) or (13 <= y <= 15 and 10 <= x <= 19)
            if not inbox:
                cat = "A: outside the drawn box (real = START menu / overworld)"
            elif y in (4, 6, 8, 10) and 6 <= x <= 14:
                cat = "B: item/field-NAME text cells (no names table this slice)"
            elif y == 14 and 12 <= x <= 15:
                cat = "C: EXIT glyph cells"
            elif y in (5, 7, 9, 11) and 14 <= x <= 16:
                cat = "D: the x + QUANTITY column"
            elif (y, x) == (11, 18):
                cat = "E: the (18,11) down-scroll marker cell"
            else:
                cat = "F: OTHER STRUCTURAL CELL"
            cats.setdefault(cat, []).append((y, x, hex(r), hex(m)))
    total_inbox = sum(1 for y in range(18) for x in range(20)
                      if (2 <= y <= 12 and 4 <= x <= 19) or (13 <= y <= 15 and 10 <= x <= 19))
    print(f"=== {tag}  (top={top} sel={sel})  in-box cells: {total_inbox} of 360")
    for c in sorted(cats):
        print(f"  {c}: {len(cats[c])} mismatched")
        for e in cats[c][:24]:
            print(f"      row {e[0]:2d} col {e[1]:2d}: real={e[2]:>5s} pokedna={e[3]:>5s}")
    if not cats:
        print("  (no mismatches at all)")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    # No guessed defaults for --grids-dir/--gb-roms: this tool can run from any
    # clone (a worktree, a standalone checkout, ...), and a plain parents[N]
    # walk silently produces a WRONG path the moment the checkout is not
    # nested exactly where one reviewer's own machine happened to have it
    # (verified while writing this: from a /tmp worktree it walked out to
    # "/private/roms/gb", a real directory-shaped path that just happens to
    # be nonsense) -- required flags fail loudly instead of guessing.
    ap.add_argument("--harness", required=True, type=Path,
                     help="path to a compiled painter harness (see README.md)")
    ap.add_argument("--grids-dir", required=True, type=Path,
                     help="dir holding the *_grid.json ground-truth files "
                          "(e.g. docs/shots/<slice>/, per review)")
    ap.add_argument("--gb-roms", required=True, type=Path,
                     help="dir holding Red.sav/Yellow.sav etc. (e.g. "
                          "gba-toolkit/roms/gb, per docs/kb/pokemon/ + roms.sh)")
    a = ap.parse_args()

    run("Red, list top (sel=0)", a.harness, a.grids_dir / "red_row1_grid.json",
        a.gb_roms / "Red.sav", 0, 0)
    run("Red, scrolled (real top=HM03 idx7, sel=BICYCLE idx9)", a.harness,
        a.grids_dir / "red_scroll8_grid.json", a.gb_roms / "Red.sav", 7, 9)
    run("Yellow, list top (sel=0)", a.harness, a.grids_dir / "yellow_row1_grid.json",
        a.gb_roms / "Yellow.sav", 0, 0)


if __name__ == "__main__":
    main()
