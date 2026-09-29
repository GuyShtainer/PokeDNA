#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""g3hp_shots.py -- the Gen-3 SKILLS-card Cur HP proof chain (BACKLOG #231, lane y8-edithp).

Boots pokedna-delta-gb.gba (`make delta-gb`), opens Emerald party slot 0 in EDIT, and drives
the HP cur/max slot: A (full -> fainted), a HP EV edit (max moves, current stays 0), RIGHT x2
(0 -> 2, the reviving edit). Frames 03 and 05 carry mechanical claims (BACKLOG #184): 03 must
show the fainted "0/240" drawn in the warning colour, 05 must show "2/241" NOT in it.

    python3 tools/g3hp_shots.py pokedna-delta-gb.gba /tmp/g3hp_out
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_claims  # noqa: E402
import gb_shots  # noqa: E402

WARN_RGB = (255, 148, 24)        # UI_WARN = RGB15(31, 18, 3)


def colour_of(frame: Path, text: str) -> tuple[int, int, int] | None:
    """Colour the first exact pixel match of `text` is drawn in on `frame`, or None."""
    hits = gb_claims.find(frame, text)
    return hits[0][2] if hits else None


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    img, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    core_mod, image_mod = gb_shots.load_mgba()
    big = gb_shots.BIG_SETTLE
    s = gb_shots.Session(core_mod, image_mod, img, out, "g3hp_")
    s.run(700)
    s.tap("A", settle=big)                       # boot picker row 0 (Emerald) -> box grid
    s.tap("UP", settle=big); s.tap("UP", settle=big)   # grid -> title -> top tabs (PARTY)
    s.tap("A", settle=big)                       # party overlay
    s.tap("A", settle=big)                       # slot 0 action menu
    s.tap("A", settle=big)                       # VIEW/EDIT -> summary card 0
    s.tap("A", settle=big)                       # A in VIEW -> editing
    s.tap("R", settle=big)                       # INFO -> SKILLS
    s.shot("01_skills_edit", "SKILLS card, EDIT mode, cursor on slot 0 (Level)")
    for _ in range(3):
        s.tap("DOWN", settle=big)                # Level -> Item -> Friend -> HP cur/max
    s.shot("02_curhp_selected", "cursor on HP cur/max slot")
    s.tap("A", settle=big)
    f3 = s.shot("03_A_fainted", "A on cur/max: full -> 0", allow_same=False,
                claim="0/240")
    s.tap("DOWN", settle=big)                    # -> HP EV slot
    for _ in range(3):
        s.tap("RIGHT", settle=big)
    s.shot("04_ev_edit_stays_fainted", "HP EV raised x3 while fainted: max moves, current stays 0")
    s.tap("UP", settle=big)                      # back to cur/max
    s.tap("RIGHT", settle=big); s.tap("RIGHT", settle=big)
    f5 = s.shot("05_revived", "RIGHT x2 on cur/max: 0 -> 2 (the reviving edit)",
                claim="2/241")
    ok = not s.any_claim_failed
    if colour_of(f3, "0/240") != WARN_RGB:
        print(f"FAIL frame 03: fainted HP not in the warning colour ({colour_of(f3, '0/240')})")
        ok = False
    if colour_of(f5, "2/241") == WARN_RGB:
        print("FAIL frame 05: revived HP still drawn in the warning colour")
        ok = False
    print(f"\n{'PASS' if ok else 'FAIL'}: {len(s.taken)} shots")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
