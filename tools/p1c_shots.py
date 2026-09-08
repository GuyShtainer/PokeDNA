#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""p1c_shots.py — headless mGBA screenshots of the Gen-1/2 trainer card, P1c
(BACKLOG #49 P1c, UX-parity: the card now LOOKS like the Gen-3 card).

Imports Session (and the KEY/HOLD/SETTLE/BIG_SETTLE constants) from
tools/gb_shots.py rather than duplicating that harness — this file only adds the
NAV MENU -> Trainer -> pdna_gbtrainer() path, same as tools/p1b_shots.py, plus a
run over an ordinary Emerald .sav (no --gb) for the side-by-side Gen-3 comparison
shot (the SAME art the GB card now reuses, tools/g3_shots.py's own fuse-a-Gen-3-
save path).

Three fused images (built by the caller):
  pokedna-delta.gba + Gold.sav --gb — the Gen-2 run: card front, card back
    (L/R flip), the badge cursor (LEFT/RIGHT + A on the front BADGES field),
    the money entry (A on MONEY).
  pokedna-delta.gba + Red.sav --gb — the Gen-1 run: same path, showing 8
    badges (not Johto/Kanto split) and no MOM/MOTHER/GENDER rows on the back.
  pokedna-delta.gba + Emerald.sav (no --gb) — the Gen-3 card itself, front +
    back, for the side-by-side "does the GB card actually look like this"
    comparison the brief asks for.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from gb_shots import Session, BIG_SETTLE, SETTLE  # noqa: E402


def load_mgba(vendor: Path | None = None):
    """Same contract as gb_shots.load_mgba() / p1b_shots.py's own override — see
    p1b_shots.py's docstring for why this worktree needs its own vendor default
    (ROOT.parent is /tmp under `git worktree add`, not .../projects)."""
    if vendor is None:
        vendor = Path("/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/rec2mp4/vendor")
    if not vendor.is_dir():
        sys.exit(f"vendor path missing: {vendor} (pass --mgba-vendor)")
    sys.path.insert(0, str(vendor))
    import mgba.core, mgba.image, mgba.log   # noqa: E402
    mgba.log.silence()
    return mgba.core, mgba.image


def run_gold_card(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #49 P1c trainer card (on the Gen-3 card art) ==")

    s.tap("A", settle=BIG_SETTLE)              # info page -> box grid
    s.tap("START", settle=BIG_SETTLE)          # box grid -> the nav menu
    s.press_n("DOWN", 3)                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)               # Trainer -> pdna_gbtrainer(), card FRONT
    s.shot("01_card_front", "#49-P1c: the Gen-1/2 trainer card FRONT, Gold/Silver/"
                             "Crystal — Emerald's own card art with NAME/ID No./MONEY/"
                             "PLAY TIME/BADGES(Johto) painted at Emerald's field "
                             "positions (pdna_trainer.h's shared card painters)")

    s.tap("R", settle=BIG_SETTLE)               # flip to the BACK
    s.shot("02_card_back", "#49-P1c: the BACK (L/R flip) — COINS, MOM'S MONEY, "
                             "MOM SAVE MODE, KANTO BADGES, RIVAL, MOTHER on Gold/"
                             "Silver's own card back (no GENDER row here — Crystal "
                             "only, see gbtr_rows.c's own comment)")

    s.tap("L", settle=BIG_SETTLE)               # back to the front (sel unchanged: ID)

    # Front cursor order is {ID, NAME, MONEY, TIME, BADGES} (CARDF_* order, card_bg.h)
    # -- ID is index 0 on entry, so DOWN x2 reaches MONEY (index 2), NOT the badge
    # row (index 4). Getting this wrong once already shipped two mislabeled shots
    # (P1c review: 02b claimed "badges selected" but the screenshot showed TIME
    # selected, and 03/04 turned out to be the SAME play-time keyboard at two
    # different keystrokes, not a badge cursor or a money entry at all -- caught by
    # actually looking at the PNGs, not by trusting this script's own comments).
    s.press_n("DOWN", 2)                        # ID -> NAME -> MONEY
    s.tap("A", settle=BIG_SETTLE)                # open num_entry("MONEY", ...)
    s.shot("03_money_entry", "#49-P1c: A on MONEY opens the same osk_search-backed "
                               "num_entry(...,999999) the Gen-3 card uses")
    s.tap("SEL", settle=BIG_SETTLE)               # osk_search: SELECT cancels

    s.press_n("DOWN", 2)                        # MONEY -> TIME -> BADGES
    s.shot("02b_badges_selected", "#49-P1c: BADGES selected on the card front — "
                                    "the red selection frame on Emerald's own badge "
                                    "row (card_field_sel_frame)")
    s.tap("RIGHT", settle=SETTLE)                # move the badge cursor one cell
    s.tap("A", settle=SETTLE)                    # toggle Johto badge 2 instantly
    s.shot("04_badge_cursor", "#49-P1c: LEFT/RIGHT moves a per-badge cursor on the "
                                "front row (8 Johto icons); A toggles instantly — "
                                "same UX as Gen 3's own card_editor badge row")

    s.tap("B", settle=BIG_SETTLE)                # B on the front: discard, back to nav menu
    return s


def run_red_card(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_red_")
    print("== Red.sav (Gen 1) -- BACKLOG #49 P1c trainer card (on the Gen-3 card art) ==")

    s.tap("A", settle=BIG_SETTLE)              # info page -> box grid
    s.tap("START", settle=BIG_SETTLE)          # box grid -> the nav menu
    s.press_n("DOWN", 3)                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)               # Trainer -> pdna_gbtrainer(), card FRONT
    s.shot("01_card_front", "#49-P1c: the Gen-1/2 trainer card FRONT, Red/Blue/"
                             "Yellow — same Emerald card art, 8 badges (Gen 1's own "
                             "single badge byte, no Johto/Kanto split)")

    s.tap("R", settle=BIG_SETTLE)               # flip to the BACK
    s.shot("02_card_back", "#49-P1c: the BACK — just COINS + RIVAL for Gen 1 (no "
                             "mom/mother/gender/Kanto rows: gbtr_build_back_rows "
                             "omits every Gen-2-only row)")

    s.tap("B", settle=BIG_SETTLE)                # B on the back: flips to front (not cancel)
    s.tap("B", settle=BIG_SETTLE)                # B on the front: discard, back to nav menu
    return s


def run_emerald_card(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """The Gen-3 card itself (no --gb save), for the side-by-side comparison."""
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_emerald_")
    print("== Emerald.sav (Gen 3) -- the ORIGINAL card, for the side-by-side comparison ==")

    s.tap("START", settle=BIG_SETTLE)           # box screen -> nav menu
    s.press_n("DOWN", 3)                        # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)                # Trainer -> pdna_trainer(), card FRONT
    s.shot("00_gen3_card_front", "#49-P1c: the ORIGINAL Gen-3 (Emerald) trainer card "
                                    "front, for comparison — this is the exact art/"
                                    "layout the Gen-1/2 card above now reuses")
    s.tap("R", settle=BIG_SETTLE)
    s.shot("00_gen3_card_back", "#49-P1c: the ORIGINAL Gen-3 (Emerald) trainer card "
                                   "back, for comparison")
    s.tap("B", settle=BIG_SETTLE)
    s.tap("B", settle=BIG_SETTLE)
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, help="pokedna-delta.gba fused with Gold.sav --gb")
    ap.add_argument("--red", type=Path, help="pokedna-delta.gba fused with Red.sav --gb")
    ap.add_argument("--emerald", type=Path,
                     help="pokedna-delta.gba fused with an ordinary Emerald.sav (no --gb) "
                          "-- the Gen-3 card itself, for the comparison shots")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "p1c")
    ap.add_argument("--mgba-vendor", type=Path, default=None,
                     help="override the mgba python vendor dir (see load_mgba() above)")
    a = ap.parse_args(argv)

    runs = [("gold", a.gold, (run_gold_card,)),
            ("red", a.red, (run_red_card,)),
            ("emerald", a.emerald, (run_emerald_card,))]
    active = [(label, p, fns) for label, p, fns in runs if p is not None]
    if not active:
        sys.exit("nothing to do: pass at least one of --gold/--red/--emerald")
    for label, p, _fns in active:
        if not p.is_file():
            sys.exit(f"--{label}: {p}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba(a.mgba_vendor)

    ok, skipped = [], []
    for _label, p, fns in active:
        for fn in fns:
            sess = fn(core_mod, image_mod, p, a.out)
            ok += sess.taken; skipped += sess.skipped

    manifest_path = a.out / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    for n, c in ok:
        by_file[n] = {"file": n, "caption": c}
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")

    print(f"\n{len(ok)} shot(s) saved to {a.out}")
    for name, reason in skipped:
        print(f"[skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
