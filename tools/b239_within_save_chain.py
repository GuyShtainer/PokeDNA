#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""b239_within_save_chain.py -- BACKLOG #239 fix-pass: prove the LEGITIMATE within-save
COPY -> PASTE chain still works after view_save() now clears g_clip on every save open.
Scratch/diagnostic script (not part of the gate) -- exploratory shots first, then the
final claim-bearing chain once the navigation is confirmed by eye.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402

ROM = ROOT / "pokedna-delta-gb.gba"


def main() -> int:
    if not ROM.is_file():
        sys.exit(f"missing {ROM}")
    core_mod, image_mod = gb_shots.load_mgba()
    out = Path("/tmp/b239_wsc_out")
    out.mkdir(exist_ok=True, parents=True)
    s = gb_shots.Session(core_mod, image_mod, ROM, out, "wsc_")

    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)        # boot picker, Emerald row -> box
    s.shot("00_box", "box screen after boot")

    s.tap("A", settle=gb_shots.BIG_SETTLE)        # cursor cell -> action menu
    s.shot("01_menu", "action menu on the default cursor cell")

    s.press_n("DOWN", 4)
    s.shot("02_menu_copy_sel", "cursor moved DOWN x4 -- should land on COPY")

    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_copied_toast", "after A on COPY -- expect the COPIED toast")

    s.tap("A", settle=gb_shots.BIG_SETTLE)        # dismiss toast
    s.shot("04_back_on_box", "back on the box screen after dismissing COPIED")

    s.tap("R", settle=gb_shots.BIG_SETTLE)        # switch to the next box, looking for an empty cell
    s.shot("05_box2", "R -- switched to the next box, looking for an empty cell")

    for i in range(6, 12):
        s.tap("R", settle=gb_shots.BIG_SETTLE)
        s.shot(f"{i:02d}_boxN", f"R again -- box switch #{i-4}")
    # box 8A (wsc_11_boxN.png) has 8/30 -- cursor is on an EMPTY cell (0,0), preview says "(empty)"

    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("12_empty_cell_menu", "action menu on the empty cell in box 8A -- expect PASTE to be offered")

    s.tap("DOWN")
    s.shot("13_paste_here_sel", "DOWN x1 -- cursor should be on PASTE HERE")

    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("14_after_paste", "after A on PASTE HERE -- verified-write busy panel expected")

    s.run(600)                                     # let the flash write + grow-in flourish finish
    s.shot("15_final", "final state after the write completes -- SAVED confirmation")

    s.tap("A", settle=gb_shots.BIG_SETTLE)          # dismiss SAVED
    s.shot("16_box_with_pasted_mon", "box 8A after dismissing SAVED -- the pasted mon should now "
           "fill the cell that was (empty) at wsc_12")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
