#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""b246_g3_to_gb_shot.py -- BACKLOG #246 / #104 Phase 1: probe/demonstration script.

Carries the planted PLAIN Gen-3 Bank cell (bank_plant_g3_box(), Bank box index 2
slot 0 -- see source/bank_plant.c's own comment) onto a Game Boy grid, from a
GB session's own Bank visit. Standalone (not wired into tools/dgb_shots.py's
regression chain, per the lane's own scope) -- this is the lane's private probe
vehicle, run once BEFORE the fix (to demonstrate the refusal) and once AFTER
(to demonstrate the new route).

    cd <worktree> && /usr/local/bin/python3 tools/b246_g3_to_gb_shot.py [--out DIR] [--tag TAG]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots             # noqa: E402
import dgb_shots            # noqa: E402

ROM = ROOT / "pokedna-delta-gb.gba"

UP_INTO_BANK = 3   # grid -> title -> tabs -> the bank_edge hop
UP_TO_ROW0 = 4     # Bank's own bottom row -> row 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("/tmp/b246_out"))
    ap.add_argument("--tag", default="")
    # BACKLOG #246 review D5: this file's own committed claim used to be the PRE-fix
    # refusal text ("NOT ACROSS GENERATIONS...") -- once the #246 D-arm landed (this
    # lane's own earlier commits), the real screen after the A-press drop is the
    # gb_paste_loss_screen/LOSS_FOOT_BRIDGE confirm ("WHAT WON'T TRANSFER" / "A =
    # transfer"), so the committed probe exited 1 on its own tree. --before restores
    # the original refusal-text claim, for anyone checking out a commit before the
    # #246 D-arm landed; the default now matches what ships today.
    ap.add_argument("--before", action="store_true",
                     help="assert the PRE-#246 refusal text instead of the post-fix "
                          "loss/confirm screen (for a checkout before the D-arm landed)")
    args = ap.parse_args()

    if not ROM.is_file():
        sys.exit(f"missing {ROM} -- run `make delta-gb` in this worktree first")
    core_mod, image_mod = gb_shots.load_mgba()
    args.out.mkdir(exist_ok=True, parents=True)
    s = gb_shots.Session(core_mod, image_mod, ROM, args.out, f"b246_{args.tag}")
    print(f"== BACKLOG #246: a plain Gen-3 Bank cell carried onto a Game Boy grid ({args.tag or 'probe'}) ==")

    dgb_shots.boot_to_gb_session(s, ROM, which="red")
    s.shot("00_red_grid", "Red's own box grid, freshly entered")

    s.press_n("UP", UP_INTO_BANK, settle=100)
    s.press_n("UP", UP_TO_ROW0, settle=60)
    s.shot("01_bank_box0", "in the Bank, box 1 (index 0) -- the native-cell plants")

    s.tap("R", settle=150)   # box 0 -> box 1
    s.tap("R", settle=150)   # box 1 -> box 2 -- bank_plant_g3_box's own box
    s.shot("02_bank_box2", "in the Bank, box 3 (index 2) -- BACKLOG #246's planted "
           "PLAIN Gen-3 cell (never native) sits at slot 0")

    s.tap("A", settle=150)   # cell action menu
    s.shot("03_cell_menu", "A on the planted Gen-3 cell -- the ordinary Gen-3 mon "
           "menu (VIEW/EDIT, ITEM, LEGALITY, MOVE, COPY, DUPLICATE, RELEASE, CANCEL) "
           "-- a plain cell, not the native whitelist")

    s.press_n("DOWN", 3, settle=80)   # VIEW/EDIT -> ITEM -> LEGALITY -> MOVE
    s.tap("A", settle=150)            # MOVE -> start_carry()
    s.shot("04_carrying", "carrying the planted Gen-3 cell, still in the Bank")

    s.press_n("DOWN", 5, settle=150)  # off the Bank's own bottom edge -> back on the GB grid
    s.shot("05_on_gb_grid_carrying", "back on Red's own GB grid, STILL CARRYING the "
           "Gen-3 cell -- about to press A to drop it")

    s.tap("A", settle=gb_shots.BIG_SETTLE)
    if args.before:
        claim = ["NOT ACROSS GENERATIONS", "This Pokemon cannot move",
                 "between these two saves yet."]
    else:
        claim = ["WHAT WON'T TRANSFER", "Kept in /PokeDNA/xfer",
                 "restored when it comes back.", "The Bank slot is emptied when it lands.",
                 "A = transfer", "B = cancel"]
    s.shot("06_drop_result", "A to drop the Gen-3 cell onto Red's GB grid -- "
           "whatever the current build does", claim=claim)

    ok = not s.any_claim_failed
    print(f"\n{'PASS' if ok else 'FAIL'}: {len(s.taken)} shots, "
          f"{'no' if ok else 'SOME'} claim failures")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
