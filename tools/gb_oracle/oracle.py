#!/usr/bin/env python3
"""Independent pixel/tilemap oracle for Game Boy (Gen-1/2) screen review
(BACKLOG #97; originated as the U4 review's own docs/shots/rvu4/oracle.py).

Boots a real Gen-1/2 ROM+save, drives to a nav target (default: START > ITEM),
and dumps:
  - LCDC / WX / WY (so tile-addressing claims are verified, not assumed)
  - the raw composed 20x18 tile-id grid (per LCDC, same composition rule as
    tools/gb_roundtrip.py's own screen_rows(), but WITHOUT the charmap step)
  - the 8x8 pixel bitmap of every distinct tile id used, resolved out of VRAM
    with the $8000/$8800 addressing mode LCDC bit 4 actually selects
  - a screenshot

This is a REAL-hardware/real-emulator ground truth generator, not a model of
PokeDNA's own painter -- it never reads any PokeDNA source file. Pair it with
celldiff.py (in this same directory), which diffs a *_grid.json this script
writes against a small host-compiled harness that runs the SHIPPED C painter
functions, cell by cell.

No ROM/save/pixel data ships in this file or in the repo -- point --rom/--sav
at your own dumps (see docs/kb/pokemon/ + roms.sh in the gba-toolkit repo)."""
import sys
import json
import argparse
import os
from pathlib import Path

# tools/gb_oracle/oracle.py -> parents[0]=gb_oracle, [1]=tools, [2]=repo root
# (the PokeDNA checkout). Kept a plain path computation, not an environment
# assumption, so this runs the same way from any clone.
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import gb_roundtrip as R

rLCDC, rSCY, rSCX, rWY, rWX = 0xFF40, 0xFF42, 0xFF43, 0xFF4A, 0xFF4B


def compose(gb):
    m = gb._core.memory.u8
    lcdc = m[rLCDC]
    scy, scx, wy, wx = m[rSCY], m[rSCX], m[rWY], m[rWX]
    bg_map = 0x9C00 if lcdc & 0x08 else 0x9800
    win_map = 0x9C00 if lcdc & 0x40 else 0x9800
    win_on = bool(lcdc & 0x20) and wy <= 143 and wx <= 166
    grid, srcs = [], []
    for y in range(18):
        row, srow = [], []
        py = y * 8
        for x in range(20):
            px = x * 8
            if win_on and py >= wy and px + 7 >= wx:
                ty = (py - wy) // 8
                tx = (px + 7 - wx) // 8
                row.append(m[win_map + (ty & 31) * 32 + (tx & 31)])
                srow.append("W")
            else:
                ty = ((scy + py) // 8) & 31
                tx = ((scx + px) // 8) & 31
                row.append(m[bg_map + ty * 32 + tx])
                srow.append("B")
        grid.append(row); srow and srcs.append(srow)
    return dict(lcdc=lcdc, scy=scy, scx=scx, wy=wy, wx=wx, bg_map=bg_map,
                win_map=win_map, win_on=win_on, grid=grid, src=srcs)


def raw_win_map(gb, base):
    m = gb._core.memory.u8
    return [[m[base + y * 32 + x] for x in range(32)] for y in range(32)]


def tile_px(gb, tid, lcdc):
    """8x8 of 2bpp values for tile id `tid` under the addressing LCDC selects."""
    m = gb._core.memory.u8
    if lcdc & 0x10:
        addr = 0x8000 + tid * 16
    else:
        addr = 0x9000 + (tid if tid < 128 else tid - 256) * 16
    out = []
    for r in range(8):
        lo, hi = m[addr + r * 2], m[addr + r * 2 + 1]
        out.append([((hi >> (7 - c)) & 1) * 2 + ((lo >> (7 - c)) & 1) for c in range(8)])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--sav", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--extra", default="",
                     help="comma keys pressed after opening the nav target, "
                          "e.g. DOWN,DOWN")
    ap.add_argument("--nav-word", default="ITEM",
                     help="the START-menu row word to navigate to before A "
                          "(default: ITEM)")
    # No guessed relative-path default here (celldiff.py's own README note
    # applies just as much: a parents[N] walk from this file silently produces
    # a WRONG path the moment the checkout is not nested exactly like one
    # reviewer's own machine) -- $GB_ROUNDTRIP_MGBA or an explicit flag only,
    # same override tools/gb_roundtrip.py itself documents.
    ap.add_argument("--mgba-vendor", default=os.environ.get("GB_ROUNDTRIP_MGBA"),
                     required="GB_ROUNDTRIP_MGBA" not in os.environ,
                     help="dir holding the mGBA Python bindings "
                          "(default: $GB_ROUNDTRIP_MGBA)")
    a = ap.parse_args()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)

    rep, gb = R._drive(a.rom, a.sav, out / "drive", want_party=False,
                       vendor=a.mgba_vendor, log=lambda *_: None)
    if rep["verdict"] != "accept":
        print("DRIVE FAILED", rep["verdict"], rep.get("reason")); return 2

    # START menu
    ok = False
    for _ in range(12):
        gb.tap("START"); gb.step(40)
        rows = gb.settle()
        if any(a.nav_word in r for r in rows):
            ok = True; break
    if not ok:
        print(f"no START menu with {a.nav_word!r}"); print("\n".join(gb.screen_rows())); return 2
    rows = gb.settle()
    print(f"[{a.tag}] START menu:")
    for i, r in enumerate(rows): print(f"   {i:2d}|{r}")

    # find the target row, move the cursor there. Gen 1/2's START menu
    # cursor is the tile 0xED in the row's first content column.
    def cursor_row():
        g = compose(gb)["grid"]
        for y in range(18):
            for x in range(20):
                if g[y][x] == 0xED:
                    return y
        return None

    def row_of(word):
        for i, r in enumerate(gb.screen_rows()):
            if word in r: return i
        return None

    target = row_of(a.nav_word)
    for _ in range(12):
        cy = cursor_row()
        if cy is None: break
        if cy == target: break
        gb.tap("DOWN" if cy < target else "UP"); gb.step(12)
    gb.tap("A"); gb.step(60); gb.settle()

    for k in [s for s in a.extra.split(",") if s]:
        gb.tap(k); gb.step(30)
    gb.settle()

    c = compose(gb)
    print(f"[{a.tag}] LCDC=0x{c['lcdc']:02X} (bit3 bgmap={'9C00' if c['lcdc']&8 else '9800'}, "
          f"bit4 addr={'$8000 unsigned' if c['lcdc']&0x10 else '$8800 signed'}, "
          f"bit5 win={'on' if c['lcdc']&0x20 else 'off'}, "
          f"bit6 winmap={'9C00' if c['lcdc']&0x40 else '9800'})  "
          f"WX={c['wx']} WY={c['wy']} SCX={c['scx']} SCY={c['scy']}")
    print(f"[{a.tag}] composed layer per cell (W=window,B=bg):")
    for y in range(18): print("   " + "".join(c['src'][y]))
    print(f"[{a.tag}] COMPOSED 20x18 tile ids:")
    for y in range(18):
        print("   [" + ",".join(f"0x{v:02x}" for v in c['grid'][y]) + "],")
    print(f"[{a.tag}] text:")
    for i, r in enumerate(gb.screen_rows()): print(f"   {i:2d}|{r}")

    gb.screenshot(out / f"REAL_{a.tag}.png", scale=3)

    # blink sampling: 120 single frames, record every cell that ever changes
    base = [r[:] for r in c['grid']]
    changed = {}
    for f in range(120):
        gb.step(1)
        g2 = compose(gb)['grid']
        for y in range(18):
            for x in range(20):
                if g2[y][x] != base[y][x]:
                    changed.setdefault((y, x), set()).add(g2[y][x])
    print(f"[{a.tag}] cells that CHANGE over 120 frames (blinking):")
    for (y, x), vals in sorted(changed.items()):
        print(f"   (row {y}, col {x}) base=0x{base[y][x]:02x} also={[hex(v) for v in sorted(vals)]}")
    if not changed: print("   (none)")

    tiles = {}
    for y in range(18):
        for x in range(20):
            t = c['grid'][y][x]
            if t not in tiles: tiles[t] = tile_px(gb, t, c['lcdc'])
    json.dump(dict(meta={k: c[k] for k in ("lcdc","scy","scx","wy","wx","bg_map","win_map")},
                   grid=c['grid'], src=c['src'],
                   winmap_raw=raw_win_map(gb, c['win_map']),
                   bgmap_raw=raw_win_map(gb, c['bg_map']),
                   tiles={str(k): v for k, v in tiles.items()}),
              open(out / f"{a.tag}.json", "w"))
    print(f"[{a.tag}] wrote {out}/{a.tag}.json  ({len(tiles)} distinct tiles)")
    return 0


# BACKLOG #97 follow-up: guarded so celldiff.py --demo can `import oracle` and
# reuse compose()/tile_px() (the same LCDC-aware VRAM ground truth this file's
# own --tag runs use) without argparse's `required=True` flags aborting the
# import (they used to -- `sys.exit(main())` ran unconditionally at module
# scope, so anything importing this file for its functions instead of running
# it as a script got a SystemExit: 2 before a single line of its own code ran).
if __name__ == "__main__":
    sys.exit(main())
