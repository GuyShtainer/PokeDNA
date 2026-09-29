#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""p1d_shots.py -- headless mGBA re-shoot for the P1c review fix pass (P1d,
BACKLOG #49). Two shots against a Gold.sav --gb fused image:

  p1c_gold_01_card_front.png  -- re-shoot of P1c's own 01_card_front: the red
    selection frame now walks around the WHOLE name (fixes the digraph-name
    frame gap) and the footer reads the Gen-3 literal "U/D A edit  L/R flip
    B save" (no more START in the legend -- P1d dropped the START key on this
    card entirely).
  p1c_gold_05_b_save_confirm.png -- B on the front (after a dirty edit) now
    asks the SAME "Save trainer changes?" confirm Gen-3's own card_editor
    asks, instead of silently discarding.

Reuses tools/gb_shots.py's Session harness the same way p1c_shots.py does.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from p1c_shots import load_mgba  # noqa: E402
from gb_shots import Session, BIG_SETTLE, SETTLE  # noqa: E402


def run_gold_card_p1d(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_gold_")
    print("== Gold.sav (Gen 2) -- P1d re-shoot: name frame + B-save flow ==")

    s.run(BIG_SETTLE)                          # #279: no info page -- the grid opens directly; this run() rides out its cold fetch
    s.tap("START", settle=BIG_SETTLE)          # box grid -> the nav menu
    s.press_n("DOWN", 3)                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)               # Trainer -> pdna_gbtrainer(), card FRONT
    s.shot("01_card_front", "#49-P1d re-shoot: the Gen-1/2 trainer card FRONT -- "
                             "the red selection frame now walks the WHOLE name "
                             "(digraph-name frame fix) and the footer reads the "
                             "Gen-3 literal 'U/D A edit  L/R flip  B save' (no "
                             "START on this card any more)")

    # ID -> NAME -> MONEY -> TIME -> BADGES: reach BADGES and toggle one, so the
    # card is dirty (t != t0) before B, the same way P1c's own run_gold_card did.
    s.press_n("DOWN", 4)
    s.tap("A", settle=SETTLE)                   # toggle a Johto badge instantly

    s.tap("B", settle=BIG_SETTLE)               # front: P1d -- B now asks to commit
    s.shot("05_b_save_confirm", "#49-P1d: B on the front (dirty: one badge "
                                  "toggled) now asks 'Save trainer changes?' -- "
                                  "the SAME confirm Gen-3's own card_editor asks, "
                                  "instead of the old silent B-discards-everything")
    s.tap("B", settle=BIG_SETTLE)                # decline the confirm: back to nav menu,
                                                   # nothing written (this is a screenshot
                                                   # run, not a save-write drill)
    return s


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, required=True,
                     help="pokedna-delta.gba fused with Gold.sav --gb")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "p1c")
    ap.add_argument("--mgba-vendor", type=Path, default=None)
    a = ap.parse_args(argv)

    if not a.gold.is_file():
        sys.exit(f"--gold: {a.gold}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba(a.mgba_vendor)
    s = run_gold_card_p1d(core_mod, image_mod, a.gold, a.out)
    print(f"\n{len(s.taken)} shot(s) saved to {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
