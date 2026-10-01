#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""zf313_bag_shots.py -- BACKLOG #313 bag chain: boot ONE fused artless image on the box grid,
open the Bag (START, DOWN x7, A), then walk every pocket (R x4), scroll (DOWN x2, UP) and come
back (L x4), taking a frame after each step and printing a per-tap trace line.
Reuses tools/gb_shots.py's Session. Usage:
    /usr/local/bin/python3 tools/zf313_bag_shots.py FUSED.gba OUTDIR PREFIX
"""
from __future__ import annotations
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gb_shots  # noqa: E402

def main(argv):
    img, out, prefix = Path(argv[1]), Path(argv[2]), argv[3]
    gb_shots.assert_vehicle(img, "ARTLESS")
    out.mkdir(parents=True, exist_ok=True)
    core_mod, image_mod = gb_shots.load_mgba()
    s = gb_shots.Session(core_mod, image_mod, img, out, prefix)
    n = [0]
    def tap(key, settle=gb_shots.SETTLE):
        s.tap(key, settle=settle)
        n[0] += 1
        print(f"  tap {n[0]:02d} {key:5s} settle={settle:3d} frame={s.core.frame_counter}")
    tap("START", gb_shots.BIG_SETTLE)
    for _ in range(7): tap("DOWN")
    tap("A", gb_shots.BIG_SETTLE)
    s.run(300)
    s.shot("00_bag_open", "bag open, pocket 0 (Items)")
    for i, name in enumerate(["Key Items", "Poke Balls", "TMs&HMs", "Berries"], 1):
        tap("R", gb_shots.BIG_SETTLE); s.run(200)
        s.shot(f"{i:02d}_pocket_{i}", f"pocket {i} ({name}) after R x{i}")
    tap("DOWN"); tap("DOWN"); s.run(60)
    s.shot("05_scroll", "pocket 4 (Berries), cursor DOWN x2")
    tap("L", gb_shots.BIG_SETTLE); s.run(200)
    s.shot("06_back_pocket_3", "back to pocket 3 (L)")
    tap("L", gb_shots.BIG_SETTLE); tap("L", gb_shots.BIG_SETTLE); tap("L", gb_shots.BIG_SETTLE); s.run(200)
    s.shot("07_back_pocket_0", "back to pocket 0 (L x3 more)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
