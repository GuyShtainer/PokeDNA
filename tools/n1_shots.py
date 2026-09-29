#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""n1_shots.py — headless mGBA screenshots of BACKLOG #58 (one identical START menu
in every game, honest per-row messages).

Reuses tools/gb_shots.py's Session/load_mgba primitives (imported, not copied) over
THREE fused pokedna-delta.gba images:

  --gold      a Gen-2 Game Boy save (Gold.sav, gb12's own fork, pdna_gen12_show_image)
              -- the row that goes through source/nav_avail.h's rule table for real:
              gb_nav_from_start (source/pdna_gen12.c) now opens NAV_ALL_AVAILABLE (no
              dimming) and routes every row but Settings/Trainer/Back through the new
              app_nav_refuse(), so Bag shows "COMING SOON" and Blocks shows
              "NOT IN GEN 2" -- both sourced from nav_avail_why(), not a hand-typed
              string in this script.
  --firered   an ordinary Gen-3 FRLG save (roms/FireRed.sav) -- the plain box screen's
              own nav switch (view_save, source/pdna_main.c), same fused-save path
              gb_shots.py's own run_e4_settings() already uses for a Gen-3 save.
  --emerald   an ordinary Gen-3 Emerald save -- for the side-by-side: Guy's ask was
              "the same start button menu in all games", so this shot exists to be
              compared pixel-for-pixel against Gold's own menu shot, not to exercise
              any refusal.

IMPORTANT FINDING (source/nav_avail.c's own file header has the receipts): every
Gen-3 kind (RS/EM/FRLG) answers NAV_OK for EVERY row in nav_avail() -- verified, not
assumed. Checking what pdna_pokeblock()/pdna_secretbase()/pdna_mirage()/pdna_clock()/
pdna_battle_record()/pdna_frontier() already do per game found every one of them
already honest (Ruby/Sapphire's Frontier row REDIRECTS to the real Battle Tower
screen; the rest already refuse on exactly the right game with their own message).
So on FireRed, Blocks does NOT show a "NOT IN FRLG" dialog from the new
app_nav_refuse() path -- nothing gates it before dispatch -- it falls straight into
pdna_pokeblock(), which shows its OWN pre-existing "NO POKEBLOCKS / This game lacks
contests." message, unchanged by this item. That is still the right, honest answer
Guy asked for (never silently do nothing), it is just not NEW code producing it. The
shots and captions below say exactly what fired, not what a first-pass guess expected.

Usage:
    /usr/local/bin/python3 tools/n1_shots.py \\
        --gold FUSED_GOLD.gba --firered FUSED_FIRERED.gba --emerald FUSED_EMERALD.gba \\
        --out docs/shots/gb

Frame-advance idiom: identical to gb_shots.py (short hold + settle; a BIG_SETTLE after
anything that opens/closes a whole screen) -- see that file's own docstring for why.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# tools/ has no __init__.py; running this file as `python3 tools/n1_shots.py` already
# puts tools/ on sys.path[0] (Python's own script-directory rule), but this script is
# also imported by test/CI harnesses that could run it from elsewhere -- belt and
# braces, matching how gb_shots.py itself inserts the mGBA vendor path below.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gb_shots import SETTLE, BIG_SETTLE, Session, load_mgba  # noqa: E402


# ---- (a) Gold.sav, Gen 2: the row that actually runs through app_nav_refuse() -------

def run_gold_menu(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "n1gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #58: identical menu, honest per-row refusals ==")

    s.run(BIG_SETTLE)                       # #279: no info page -- the grid opens directly; this run() rides out its cold fetch
    s.tap("START", settle=BIG_SETTLE)       # box grid -> the nav menu
    s.shot("01_menu", "#58: Gold (Gen 2) START menu -- the SAME panel/labels/order as "
                       "a Gen-3 save's own (no dimming; compare against em_01_menu)")

    # BACKLOG #251 shots-refresh re-derivation (was: "PDNA_NAV_ITEMS index 7 = NV_BAG
    # ... DOWN x7 ... Bag -> app_nav_refuse -> COMING SOON"): source/nav_avail.c's own
    # GB_TABLE now answers NAV_OK for NV_BAG on BOTH Gen 1 and Gen 2 (U4/U5 landed
    # Red/Yellow's own Item bag and Gold/Silver/Crystal's own Pack for real since this
    # was last calibrated) -- pressing A on Bag now opens the REAL Pack screen, not a
    # refusal, and this function's OLD follow-up taps (a dismiss-A, then START to
    # reopen the nav menu) instead operated INSIDE that live screen, drifting into an
    # item's own quantity keyboard -- reproduced live, caught by looking at the actual
    # captured frames (a Bag item list and a "QUANTITY" OSK), not trusting the tap
    # count. Bag can no longer demonstrate a refusal at all (it is a real, working
    # screen now, which is itself worth noting). Swapped the "still NAV_COMING_SOON"
    # example to NV_XFER ("Transfers" -- GB_TABLE still answers NAV_COMING_SOON /
    # "Open it from a Gen-3 save." for a raw Game Boy session, unaffected by U4/U5).
    # PDNA_NAV_ITEMS index 18 = NV_XFER, column 1 (index>=PDNA_NAV_ROWS=11) row 7 --
    # one RIGHT (whole-column jump to index 11, NV_EVENTS) then DOWN x7.
    s.press_n("RIGHT", 1, settle=SETTLE)    # Party -> Tickets (column 1 top)
    s.press_n("DOWN", 7, settle=SETTLE)     # Tickets -> ... -> Transfers (index 18)
    s.tap("A", settle=BIG_SETTLE)           # Transfers -> app_nav_refuse(NV_XFER, SE_KIND_GEN2)
    s.shot("02_bag_coming_soon", "#58: Gold, Transfers -- \"COMING SOON\" / \"Open it from a "
                                  "Gen-3 save.\" (nav_avail_why, not a screen-local string) -- "
                                  "re-targeted off NV_XFER: NV_BAG (this shot's original subject) "
                                  "is NAV_OK on both generations now (U4/U5 landed Red/Yellow's "
                                  "own Item bag + Gold/Silver/Crystal's own Pack for real), so "
                                  "Bag can no longer demonstrate a COMING-SOON refusal at all")
    s.tap("A", settle=BIG_SETTLE)           # dismiss msg_wait -> back to the box grid

    s.tap("START", settle=BIG_SETTLE)       # reopen the nav menu (sel resets to 0)
    # BACKLOG #251 shots-refresh re-derivation (was: "PDNA_NAV_ITEMS index 10 =
    # NV_POKEBLOCK, the first row of column 1 -- one RIGHT ... lands on it directly"):
    # source/pdna_layout.h's PDNA_NAV_ITEMS has since grown NV_MAP/NV_GB/NV_XFER,
    # raising PDNA_NAV_ROWS from 10 to 11 -- NV_POKEBLOCK (index 10) is now the LAST
    # row of column 0, not the first row of column 1, so a plain DOWN x10 from the
    # menu's default sel=0 reaches it directly; RIGHT would now overshoot to index 11
    # (NV_EVENTS, "Tickets") instead. Reproduced live against the real Sprites-grid-
    # style frame, same fix class as gb_shots.py's own run_e4_settings().
    s.press_n("DOWN", 10, settle=SETTLE)    # Party -> ... -> Blocks (index 10, col 0 last row)
    s.tap("A", settle=BIG_SETTLE)           # Blocks -> app_nav_refuse(NV_POKEBLOCK, SE_KIND_GEN2)
    s.shot("03_blocks_not_in_gen2", "#58: Gold, Blocks -- \"NOT IN GEN 2\" / \"Gen 2 games "
                                     "have no Blocks.\" (Gen 2 never had Pokeblocks)")
    return s


# ---- (b) FireRed.sav, Gen 3 FRLG: the pre-existing per-game refusals, undimmed ------

def run_firered_menu(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "n1fr_")
    print("== FireRed.sav (Gen 3, FRLG) -- BACKLOG #58: identical menu, EXISTING per-game "
          "refusals still fire (verified: nav_avail's Gen-3 gate list is empty) ==")

    # BACKLOG #251 shots-refresh re-derivation (was: "RIGHT x1 -> Blocks (index 10,
    # col 1 row 0)"): same PDNA_NAV_ROWS 10->11 shift as run_gold_menu()'s own fix
    # above -- NV_POKEBLOCK (index 10) is now column 0's OWN last row, reached by a
    # plain DOWN x10, not a column-jump RIGHT (which now overshoots to index 11,
    # NV_EVENTS/"Tickets" -- reproduced live, this exact frame used to be captioned
    # "Blocks" while actually showing the Event Tickets screen).
    s.tap("START", settle=BIG_SETTLE)       # box screen (no info page for a Gen-3 fused save)
    s.press_n("DOWN", 10, settle=SETTLE)    # Party -> ... -> Blocks (index 10, col 0 last row)
    s.tap("A", settle=BIG_SETTLE)           # Blocks -> pdna_pokeblock()'s OWN gate fires
                                             # (pk_pokeblock_offset(PK_FRLG) == 0) -- nav_avail
                                             # never intercepts this row for a Gen-3 kind
    s.shot("01_blocks_no_contests", "#58: FireRed, Blocks -- the row is undimmed and "
                                     "reachable, and pdna_pokeblock()'s OWN existing check "
                                     "(pk_pokeblock_offset==0) shows \"NO POKEBLOCKS\" / "
                                     "\"This game lacks contests.\" -- NOT a new "
                                     "\"NOT IN FRLG\" dialog: this screen already refused "
                                     "correctly, so nav_avail.c never gates it (see its "
                                     "own file header)")
    s.tap("A", settle=BIG_SETTLE)           # dismiss -> box grid

    # BACKLOG #251 shots-refresh re-derivation (was: "RIGHT x1 -> Blocks (index 10) ...
    # DOWN x3 -> Tickets -> Records -> Frontier (index 13)"): NV_FRONTIER is index 13,
    # column 1 (13 >= PDNA_NAV_ROWS=11) row 13-11=2 -- one RIGHT (column jump to index
    # 11, NV_EVENTS) then DOWN x2, not DOWN x3 off an index-10 start that no longer
    # exists in column 1 at all.
    s.tap("START", settle=BIG_SETTLE)       # reopen the nav menu
    s.press_n("RIGHT", 1, settle=SETTLE)    # Party -> Tickets (column 1 top, index 11)
    s.press_n("DOWN", 2, settle=SETTLE)     # Tickets -> Records -> Frontier (index 13)
    s.tap("A", settle=BIG_SETTLE)           # Frontier -> pdna_frontier()'s OWN g3f_supported()
                                             # gate fires (FRLG has no streak block at all)
    s.shot("02_frontier_no_records", "#58: FireRed, Frontier -- pdna_frontier()'s OWN "
                                      "existing check (g3f_supported(PK_FRLG)==false) shows "
                                      "\"FRONTIER\" / \"This game has no Battle Frontier "
                                      "records.\" -- same reason: already honest, so not "
                                      "gated a second time (Ruby/Sapphire's OWN Frontier row "
                                      "redirects to the real Battle Tower screen instead of "
                                      "refusing anything at all -- also left unGated)")
    return s


# ---- (c) Emerald.sav, Gen 3: the side-by-side reference ------------------------------

def run_emerald_menu(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "n1em_")
    print("== Emerald.sav (Gen 3) -- BACKLOG #58: the menu, for the side-by-side ==")

    s.tap("START", settle=BIG_SETTLE)
    s.shot("01_menu", "#58: Emerald (Gen 3) START menu -- compare pixel-for-pixel "
                       "against n1gold_01_menu; same panel, same 21 labels, same order "
                       "(BACKLOG #251 shots-refresh: was 19 when this caption was "
                       "written -- source/pdna_layout.h's PDNA_NAV_ITEMS has since "
                       "grown NV_MAP/NV_GB/NV_XFER, re-counted live off this exact "
                       "frame, not assumed)")
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, help="pokedna-delta.gba fused with a Gen-2 .sav (--gb)")
    ap.add_argument("--firered", type=Path,
                     help="pokedna-delta.gba fused with an ordinary FRLG .sav")
    ap.add_argument("--emerald", type=Path,
                     help="pokedna-delta.gba fused with an ordinary Emerald .sav")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb",
                     help="same shared dir gb_shots.py writes to (one manifest.json)")
    a = ap.parse_args(argv)

    runs = [("gold", a.gold, run_gold_menu),
            ("firered", a.firered, run_firered_menu),
            ("emerald", a.emerald, run_emerald_menu)]
    active = [(label, p, fn) for label, p, fn in runs if p is not None]
    if not active:
        sys.exit("nothing to do: pass at least one of --gold/--firered/--emerald")
    for label, p, _fn in active:
        if not p.is_file():
            sys.exit(f"--{label}: {p}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba()

    ok, skipped = [], []
    for _label, p, fn in active:
        sess = fn(core_mod, image_mod, p, a.out)
        ok += sess.taken; skipped += sess.skipped

    # Same merge-safe manifest.json gb_shots.py's own main() writes -- keyed by
    # filename, so a re-run of just one of --gold/--firered/--emerald updates only
    # its own entries and never drops the other lanes' (g1/b2) shots already there.
    import json
    manifest_path = a.out / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    # BACKLOG #251 shots-refresh fix: Session.taken entries are (name, caption,
    # claim_info) since gb_shots.py's BACKLOG #184 claim mechanism -- this loop still
    # unpacked 2 values and crashed AFTER every shot() call had already run (the PNGs
    # were fine; only the manifest write -- and therefore this script's OWN entries
    # ever reaching gb_contact_sheet.py -- never happened). claim_info is merged in
    # verbatim, same as gb_shots.py's own main() does.
    for n, c, claim_info in ok:
        entry = {"file": n, "caption": c}
        entry.update(claim_info)
        by_file[n] = entry
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
