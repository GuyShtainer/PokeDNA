#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""zk422_star_shots.py -- BACKLOG #422 chain: a pre-#408 Emerald save (5 museum records, no
PAINTING_MADE flags) fused onto the private delta-artless vehicle. Nav: box grid -> START ->
DOWN x3 (Trainer) -> A -> UP x1 (artless plain page, TF_NAME=0 wraps to TF_STARS=7) -> A -> CARD STARS editor, then walk the
museum row (DOWN x2) and press A three times. Per-tap trace is printed.
Reuses tools/gb_shots.py's Session. Usage:
    /usr/local/bin/python3 tools/zk422_star_shots.py FUSED.gba OUTDIR PREFIX
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
    for _ in range(3): tap("DOWN")                 # Party, Bank, Daycare -> Trainer
    s.shot("a_nav_trainer", "nav menu, cursor on Trainer (DOWN x3)")
    tap("A", gb_shots.BIG_SETTLE); s.run(200)
    s.shot("b_card", "trainer card front")
    tap("UP")                                      # NAME wraps to STARS
    s.shot("c_card_stars_sel", "plain trainer page, STARS row selected (UP x1)")
    tap("A", gb_shots.BIG_SETTLE); s.run(120)
    s.shot("00_editor_open", "CARD STARS editor open, cursor row 0")
    tap("DOWN"); tap("DOWN"); s.run(60)
    s.shot("01_museum_row", "cursor on the museum row (DOWN x2)")
    tap("A", gb_shots.BIG_SETTLE); s.run(60)
    s.shot("02_after_heal", "after A #1")
    tap("A", gb_shots.BIG_SETTLE); s.run(60)
    s.shot("03_after_off", "after A #2")
    tap("A", gb_shots.BIG_SETTLE); s.run(60)
    s.shot("04_after_on", "after A #3")
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
