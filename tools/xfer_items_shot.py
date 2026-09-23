#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""xfer_items_shot.py -- xfer-items fix pass F7: the shot chain F1/F3's own fix never
got. Modelled directly on tools/b246_g3_to_gb_vsd.py's own structure (same Session/
--vsd idiom), but on the single `make delta-gb` ROM (pokedna-delta-gb.gba) via
tools/dgb_shots.py's boot_to_gb_session(), which is simpler than run_s150_8_bridge's
own two-separate-fused-image setup and reaches the SAME shared Bank (box0/1/2 plants
are Gen-3-space, not tied to which GB save you view them from).

Scene A (F1): the GB1<->GB2 bridge's item/Secret-ID row. source/bank_plant.c's
bank_plant_gen2_badmoves_cell() (Bulbasaur/BADMOVE, box 1 slot 7 -- the one planted
cell that both clears the Gen-1 species floor, dex 1, AND reaches gb_paste_loss_screen
on a bridge, per BACKLOG #212 review D3(b)/run_s150_8_bridge's own docstring) now also
holds item id 19 (this lane's F7 fixture change) -- carried from Gold's Bank into Red,
bank_down_convert.c's own decision 15 means a Gen-2 item can NEVER reach Gen 1 on this
arm, so the loss screen's item row unconditionally reads "stays behind" (pre-F1: this
row was SILENT, because loss.item_outcome stays G3GB_ITEM_NONE on the bridge arm and
the old predicate only checked item_outcome/secret_id -- see F1's own commit).

Scene B (F3 attempt, case B "-> bag"): a plain Gen-3 Bulbasaur (bank_plant_g3_box(),
Bank box 2 slot 0) carried into Red -- gen3_to_gb_fixed()'s own G3GB_ITEM_* ladder.
This fixture holds NO item today (plant_g3_pair_bulbasaur() never calls
gb_set_held_item/sets an item field) and this lane did NOT add one -- box 2's fixture
is shared by BACKLOG #246's whole existing shot-chain surface (bank_plant_g3_box's own
header comment: "every existing shot chain's pixel captions and counts key off
bank_plant_box0()/bank_plant_box_full()'s own byte-for-byte content, and this lane
must not move either" -- the SAME rule applies to box 2's content, and this repo's
dgb_shots.py is too large (9000+ lines) to audit every consumer within this lane's own
scope. Scene B is therefore SKIPPED here, honestly, not faked -- see the lane's own
report for what this costs and the backlog line it becomes.

    cd <worktree> && /usr/local/bin/python3 tools/xfer_items_shot.py [--out DIR]
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots             # noqa: E402
import dgb_shots            # noqa: E402

ROM = ROOT / "pokedna-delta-gb.gba"

UP_INTO_BANK = 3
UP_TO_ROW0 = 4
DOWN_OFF_BANK = 5


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("/tmp/xfer_items_shot_out"))
    args = ap.parse_args()

    if not ROM.is_file():
        sys.exit(f"missing {ROM} -- run `make delta-gb` in this worktree first")

    core_mod, image_mod = gb_shots.load_mgba()
    args.out.mkdir(exist_ok=True, parents=True)

    s = gb_shots.Session(core_mod, image_mod, ROM, args.out, "xferitems_")
    print("== xfer-items fix pass F7: the shot chain F1/F3's own fix never got ==")

    dgb_shots.boot_to_gb_session(s, ROM, which="red")

    # ---- Scene A (F1): the bridge's item/Secret-ID row -----------------------------
    s.press_n("UP", UP_INTO_BANK, settle=100)
    s.press_n("UP", UP_TO_ROW0, settle=60)
    s.shot("00_bank_box0", "Red's own Bank, box 0 (the shared, generation-agnostic "
           "plant), cursor on slot 0")
    s.tap("R", settle=150)
    s.shot("01_bank_box1", "R x1 -> box 1 (BANK 2, bank_plant_box_full's 30-cell "
           "fixture), cursor on slot 0")
    s.tap("DOWN", settle=150)
    s.tap("RIGHT", settle=150)
    s.shot("02_cursor_badmoves_item", "DOWN x1, RIGHT x1 (COLS=6 wraps within the "
           "row) -> slot 7, the planted BULBASAUR (dex 1, OT BADMOVE/9999) -- F7's "
           "own fixture change: now also holds item id 19 (ESCAPE ROPE)")
    s.tap("A", settle=150)                        # native-cell whitelist menu
    s.tap("DOWN", settle=60)                       # VIEW/EDIT -> MOVE
    s.tap("A", settle=150)                         # MOVE -> carrying
    s.press_n("DOWN", DOWN_OFF_BANK, settle=150)
    s.shot("03_carrying", "carrying the BULBASAUR+item cell, back on Red's own "
           "(Gen-1) grid -- BOX1, 20/20 (no room here; see the R x5 below)")
    s.press_n("R", 5, settle=150)
    s.shot("04_box5_room", "R x5 -> box 5 (room, matches run_s150_8_bridge's own "
           "corpus finding for this same Red.sav) -- ready to drop")
    s.tap("A", settle=300)                         # bdc_convert_gb_core runs -> loss screen
    s.shot("05_loss_screen_item_row", "A -- gb_paste_loss_screen's 'WHAT WON'T "
           "TRANSFER' preview, F1's own fix: the item row now reads the generic "
           "'Held item and Secret ID' text (PDNA_SIDECAR_LOSS_ITEMSECRET) -- "
           "item_outcome stays G3GB_ITEM_NONE on the bridge arm, so loss_item_text "
           "takes its NONE/default branch, NOT a per-item 'X stays behind' line -- "
           "and the row was SILENT before F1 (the old predicate never checked "
           "loss.item_dropped)", claim="Held item and Secret ID")
    s.tap("B", settle=gb_shots.BIG_SETTLE)         # cancel -- this vehicle's own
                                                    # gb_persist() PDNA_DELTA wall
                                                    # (see b246_g3_to_gb_vsd.py's own
                                                    # docstring) is not this lane's
                                                    # own scope to re-demonstrate
    s.shot("06_after_cancel", "B = cancel on the loss screen -- back on the box grid. "
           "Found live, not assumed (an earlier draft of this caption guessed 'still "
           "carrying'): no 'A drop / B cancel' legend is visible in this frame, unlike "
           "frames 03/04, so B on THIS screen appears to end the carry outright rather "
           "than just refuse the one drop -- not further investigated within this "
           "lane's own scope (F7 only needs frame 05's loss screen).")

    print()
    print("Scene B (case B '-> bag') and a Secret-ID row: NOT staged -- see this "
          "file's own module docstring for why (bank_plant_g3_box's box-2 fixture is "
          "shared by BACKLOG #246's whole existing shot-chain surface; adding an item "
          "to it was judged too large an audit for this lane's own scope). Case C "
          "('-> item PC') was not attempted for the same reason, one level further.")

    print(f"\n{len(s.taken)} shot(s), {len(s.skipped)} skip(s)")
    for name, reason in s.skipped:
        print(f"  [skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
