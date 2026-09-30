#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""p1c_shots.py — headless mGBA screenshots of the Gen-1/2 trainer card (BACKLOG
#49 P1c, re-vehicled by BACKLOG #287) beside the Gen-3 card it was modelled on.

WHAT THIS PROVES NOW (BACKLOG #287, 2026-09-30)
  The Emerald-art Gen-1/2 trainer card this file was written for (P1c, "the card now
  LOOKS like the Gen-3 card") was RETIRED on 2026-09-10 by 8bedceb + U3: with a Game Boy
  ROM registered/fused, Trainer opens the GB-SCREEN SHELL card over that ROM
  (pdna_gbtrainer_gen1_card()/_gen2_card(), the game's own font/frame/badge tiles); with
  no ROM it falls back to the plain "GB ART: OFF" list page (covered by tools/
  p1b_shots.py). So the Gold and Red legs here now run on ROM-FUSED images and shoot the
  LIVE shell card. What each leg proves:
    Red   — Gen-1 card: one page (NAME/MONEY/TIME + the 8-badge grid), cursor moves,
            LEFT/RIGHT walks the badge cursor, A toggles a badge (nothing is written).
    Gold  — Gen-2 card: page 1 (NAME/ID/MONEY + STATUS: dex/play time), the MONEY
            editor (num_entry), RIGHT flips to page 2 (the 8 Johto badge faces),
            A toggles a badge.
    Emerald — the untouched Gen-3 card, front + back, for the side-by-side (unchanged).
  Dropped with the retired card (8bedceb): the "BACK" screen (L/R flip; COINS, MOM'S
  MONEY, MOM SAVE MODE, KANTO BADGES, RIVAL, MOTHER rows) — R/L do nothing on the shell
  card (Gen 2's page flip is LEFT/RIGHT, Gen 1 has one page), and those rows now live only
  on the plain fallback page (p1b_shots.py).

VEHICLES (the caller builds them from a `make delta` pokedna-delta.gba)
  Gold and Red each need a SINGLE-ROM image, because the captions' claim_gb= text checks
  read the embedded ROM's own font (gb_shots.Session._gb_rom_file()) and refuse a
  multi-ROM image:
    tools/fuse_gb.py pokedna-delta.gba roms/gb/Red.gb  roms/gb/Red.sav  -o red1.gba --force
    tools/fuse_gb.py pokedna-delta.gba roms/gb/Gold.gbc roms/gb/Gold.sav -o gold1.gba --force
    tools/fuse_sav.py pokedna-delta.gba roms/Emerald.sav -o em1.gba --force   (Gen-3 leg)
  A one-ROM image has no boot picker (gb_delta_pick_save()'s `n == 1` shortcut);
  `make delta-gb`'s combined image also works for the navigation (dgb_shots.
  boot_to_gb_session() picks the row) but its claim_gb= checks cannot run on it.
  Post-#279 entry: the box grid opens DIRECTLY (no info page) — boot_to_gb_session()
  rides out the cold ROM scan, then START -> nav menu -> DOWN x3 -> Trainer -> A. The
  shell's own first open is another cold whole-ROM scan (GB_ART_COLD_SETTLE).

Imports Session (and the SETTLE/BIG_SETTLE constants) from tools/gb_shots.py and the
boot/settle helpers from tools/dgb_shots.py rather than duplicating either harness.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from gb_shots import Session, BIG_SETTLE, SETTLE  # noqa: E402
from dgb_shots import GB_ART_COLD_SETTLE, assert_screen, boot_to_gb_session  # noqa: E402


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


def _open_gb_trainer_card(s: Session, which: str) -> None:
    """Boot the one-ROM image `which` to its box grid (#279: direct entry), then
    START -> nav menu -> DOWN x3 (Party, Bank, Daycare, Trainer) -> A. The shell's
    first open is its own cold whole-ROM scan, hence GB_ART_COLD_SETTLE."""
    boot_to_gb_session(s, s.rom_path, which=which)
    s.tap("START", settle=BIG_SETTLE)           # box grid -> the nav menu
    s.press_n("DOWN", 3)                        # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=GB_ART_COLD_SETTLE)       # Trainer -> the GB-shell card (gbscr_open)


def run_gold_card(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """`rom` must be a ONE-ROM Gold image (see the module docstring)."""
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_gold_")
    print("== Gold (Gen 2) -- BACKLOG #287: the LIVE GB-shell trainer card ==")

    _open_gb_trainer_card(s, "gold")
    s.shot("01_card_front", "#287: Gold's LIVE trainer card, page 1 (GB-screen shell over "
                            "the fused ROM, 8bedceb+U3) -- NAME/, ID No and MONEY in the "
                            "game's own font, the trainer pic, and the STATUS panel with "
                            "POKeDEX and PLAY TIME (claims: only the strings the "
                            "matcher can read on this frame -- the STATUS title and the "
                            "BADGES hint are not on the gbscr text grid)",
           claim_gb=["NAME/", "MONEY", "PLAY TIME"])

    s.press_n("DOWN", 2)                        # NAME -> ID -> MONEY (G2C_* cursor order)
    s.tap("A", settle=BIG_SETTLE)               # A on MONEY: num_entry("MONEY", ...)
    s.shot("02_money_entry", "#287: A on MONEY opens the same osk_search-backed "
                             "num_entry(...,999999) the Gen-3 card uses")
    s.tap("SEL", settle=BIG_SETTLE)             # osk_search: SELECT cancels (B deletes)

    s.tap("RIGHT", settle=BIG_SETTLE)           # page 1 -> page 2 (the Gen-2 card's "back")
    s.shot("03_page2_badges", "#287: RIGHT flips to page 2 -- NAME/MONEY stay on top, the "
                              "STATUS panel is replaced by the 8 gym-leader badge cells "
                              "with the cursor on the first (this replaces the retired "
                              "card's R-flip BACK screen, 8bedceb; the flip itself is "
                              "proven by the frame-differs guard, the top rows by claim_gb)",
           claim_gb=["NAME/", "MONEY"])

    s.tap("A", settle=BIG_SETTLE)               # toggle the badge under the cursor (badge 0)
    s.shot("04_badge_toggled", "#287: A toggles badge 0 -- only that cell's pixels change "
                               "(local edit only, nothing is written)")

    s.tap("B", settle=BIG_SETTLE)               # B with a REAL edit -> 'Save trainer changes?'
    s.tap("B", settle=BIG_SETTLE)               # decline -> the edit is discarded -> box grid
    assert_screen(s, "gb_box_grid")
    return s


def run_red_card(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """`rom` must be a ONE-ROM Red image (see the module docstring)."""
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_red_")
    print("== Red (Gen 1) -- BACKLOG #287: the LIVE GB-shell trainer card ==")

    _open_gb_trainer_card(s, "red")
    s.shot("01_card_front", "#287: Red's LIVE trainer card (GB-screen shell over the "
                            "fused ROM, 8bedceb+U3) -- NAME/ MONEY/ TIME/ in the game's "
                            "own font, the player pic, and the BADGES box with 8 badges "
                            "(Gen 1's single badge byte)",
           claim_gb=["NAME/", "MONEY/", "TIME/", "BADGES"])

    s.tap("DOWN", settle=SETTLE)                # cursor NAME -> MONEY (G1C_* order)
    s.shot("02_cursor_money", "#287: DOWN moves the selection frame from NAME to MONEY")

    s.press_n("DOWN", 2)                        # MONEY -> TIME -> the first badge
    s.tap("RIGHT", settle=SETTLE)               # badge cursor: badge 0 -> badge 1
    s.shot("03_badge_cursor", "#287: on a badge, LEFT/RIGHT walks the per-badge cursor "
                              "(one cell right of the first badge here)")

    s.tap("A", settle=BIG_SETTLE)               # instant toggle of that badge
    s.shot("04_badge_toggled", "#287: A toggles the badge under the cursor -- its cell swaps "
                               "from the badge tile to a face tile (unowned); local edit "
                               "only, nothing is written)")

    s.tap("B", settle=BIG_SETTLE)               # B with a REAL edit -> 'Save trainer changes?'
    s.tap("B", settle=BIG_SETTLE)               # decline -> the edit is discarded -> box grid
    assert_screen(s, "gb_box_grid")
    return s


def run_emerald_card(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """The Gen-3 card itself (no --gb save), for the side-by-side comparison."""
    s = Session(core_mod, image_mod, rom, out_dir, "p1c_emerald_")
    print("== Emerald.sav (Gen 3) -- the ORIGINAL card, for the side-by-side comparison ==")

    s.tap("START", settle=BIG_SETTLE)           # box screen -> nav menu
    s.press_n("DOWN", 3)                        # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)                # Trainer -> pdna_trainer(), card FRONT
    s.shot("00_gen3_card_front", "#49-P1c: the ORIGINAL Gen-3 (Emerald) trainer card "
                                    "front, for comparison (the Gen-1/2 card no longer "
                                    "reuses this art since 8bedceb; #287)")
    s.tap("R", settle=BIG_SETTLE)
    s.shot("00_gen3_card_back", "#49-P1c: the ORIGINAL Gen-3 (Emerald) trainer card "
                                   "back, for comparison")
    s.tap("B", settle=BIG_SETTLE)
    s.tap("B", settle=BIG_SETTLE)
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, help="ONE-ROM image: pokedna-delta.gba + Gold.gbc/Gold.sav via fuse_gb.py")
    ap.add_argument("--red", type=Path, help="ONE-ROM image: pokedna-delta.gba + Red.gb/Red.sav via fuse_gb.py")
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
    any_claim_failed = False
    for _label, p, fns in active:
        for fn in fns:
            sess = fn(core_mod, image_mod, p, a.out)
            ok += sess.taken; skipped += sess.skipped
            any_claim_failed = any_claim_failed or sess.any_claim_failed

    manifest_path = a.out / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    for n, c, claim_info in ok:   # gb_shots.Session.taken is (file, caption, claim_info)
        by_file[n] = {"file": n, "caption": c, **claim_info}
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")

    print(f"\n{len(ok)} shot(s) saved to {a.out}")
    for name, reason in skipped:
        print(f"[skip] {name}: {reason}")
    if any_claim_failed:
        print("\n[CLAIM FAILED] one or more shots -- see [CLAIM FAILED] lines above "
              "and each entry's manifest.json \"claim_failed\" list", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
