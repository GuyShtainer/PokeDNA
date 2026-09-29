#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gbhp_shots.py -- the Gen-1/2 SKILLS-card Cur HP proof chain (BACKLOG #231, lane y8-edithp).

Boots pokedna-delta-gb.gba (`make delta-gb`), opens the party (L from box 1), edits slot 0 on
the SKILLS card: A (full -> fainted), a HP stat-exp edit (current stays 0), RIGHT x2 (0 -> 2).
Frames 03 and 05 carry mechanical claims (BACKLOG #184): 03 must show the fainted "0/<max>" in
the warning colour (G5), 05 the revived "2/<max>" NOT in it. The maxima are per save.

    python3 tools/gbhp_shots.py pokedna-delta-gb.gba crystal /tmp/gbhp_out
    python3 tools/gbhp_shots.py pokedna-delta-gb.gba red /tmp/gbhp_out
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import dgb_shots  # noqa: E402
import gb_claims  # noqa: E402
import gb_shots  # noqa: E402

WARN_RGB = (255, 148, 24)        # UI_WARN = RGB15(31, 18, 3)
# party slot 0's max HP on Guy's corpus saves (Crystal Typhlosion Lv100, Red slot 0)
MAX_HP = {"crystal": 359, "red": 399}


def colour_of(frame: Path, text: str) -> tuple[int, int, int] | None:
    """Colour the first exact pixel match of `text` is drawn in on `frame`, or None."""
    hits = gb_claims.find(frame, text)
    return hits[0][2] if hits else None


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[2] not in MAX_HP:
        raise SystemExit(__doc__)
    img, which, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    mx = MAX_HP[which]
    core_mod, image_mod = gb_shots.load_mgba()
    big = gb_shots.BIG_SETTLE
    s = gb_shots.Session(core_mod, image_mod, img, out, f"gbhp_{which}_")
    dgb_shots.boot_to_gb_session(s, img, which=which)
    s.tap("L", settle=big)
    s.shot("01_L", "L from box 1")
    s.tap("A", settle=big)                       # mon menu
    s.tap("A", settle=big)                       # VIEW/EDIT -> summary
    s.tap("A", settle=big)                       # editing
    s.tap("R", settle=big)                       # -> SKILLS
    s.shot("02_skills_edit", "GB SKILLS card, EDIT, first slot")
    s.tap("A", settle=big)
    f3 = s.shot("03_A_fainted", "A on cur/max: full -> 0", claim=f"0/{mx}")
    s.tap("DOWN", settle=big)
    s.tap("LEFT", settle=big); s.tap("LEFT", settle=big)
    s.shot("04_statexp_edit_stays_fainted", "HP stat exp lowered x2 while fainted: current stays 0")
    s.tap("UP", settle=big)
    s.tap("RIGHT", settle=big); s.tap("RIGHT", settle=big)
    f5 = s.shot("05_revived", "RIGHT x2 on cur/max: 0 -> 2 (the reviving edit)", claim=f"2/{mx}")
    ok = not s.any_claim_failed
    if colour_of(f3, f"0/{mx}") != WARN_RGB:
        print(f"FAIL frame 03: fainted HP not in the warning colour ({colour_of(f3, f'0/{mx}')})")
        ok = False
    if colour_of(f5, f"2/{mx}") == WARN_RGB:
        print("FAIL frame 05: revived HP still drawn in the warning colour")
        ok = False
    print(f"\n{'PASS' if ok else 'FAIL'}: {len(s.taken)} shots")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
