#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""p1b_shots.py — headless mGBA screenshots of the Gen-1/2 trainer card (BACKLOG #49
P1b, docs/GEN12-PARITY-DESIGN.md sections 1.1, 4.1 P1, 4.4).

Imports Session (and the KEY/HOLD/SETTLE/BIG_SETTLE constants, load_mgba()) from
tools/gb_shots.py rather than duplicating that harness -- this file only adds the
NAV MENU -> Trainer -> pdna_gbtrainer() path gb_shots.py's own runs never open.

Two fused images (built by the caller, same as gb_shots.py):
  pokedna-delta.gba + Gold.sav (tools/fuse_sav.py --gb) -- the Gen-2 run: the info
    page, the box grid, START -> the nav menu -> Trainer -> the card (view), an
    edit of MONEY (the number entry), a badge toggled, then START on the card to
    commit (or the honest refusal, whichever the build actually does under mGBA --
    this script reports what happened rather than assuming).
  pokedna-delta.gba + Red.sav -- the Gen-1 run: the same path, showing no MOM
    rows and 8 badges (not 16).
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
    """Same contract as gb_shots.load_mgba(), NOT reused directly: that function
    derives the vendor path from ITS OWN file location (ROOT.parent/rec2mp4/vendor),
    which only resolves correctly when PokeDNA and rec2mp4 are sibling directories
    under projects/ -- true in the main checkout, false in a `git worktree add`
    checkout under /tmp (this slice's own worktree: ROOT.parent is /tmp, not
    .../gba-toolkit/projects). --mgba-vendor (or this default) sidesteps that
    without editing gb_shots.py, which the brief for this slice says not to touch."""
    if vendor is None:
        vendor = Path("/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/rec2mp4/vendor")
    if not vendor.is_dir():
        sys.exit(f"vendor path missing: {vendor} (pass --mgba-vendor)")
    sys.path.insert(0, str(vendor))
    import mgba.core, mgba.image, mgba.log   # noqa: E402
    mgba.log.silence()
    return mgba.core, mgba.image


def run_gold_trainer(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "p1b_gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #49 P1b trainer card ==")

    s.run(BIG_SETTLE)                          # #279: no info page -- the grid opens directly; this run() rides out its cold fetch
    s.tap("START", settle=BIG_SETTLE)          # box grid -> the nav menu
    s.press_n("DOWN", 3)                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)               # Trainer -> pdna_gbtrainer(), card view
    s.shot("01_card", "#49-P1: the Gen-1/2 trainer card, Gold/Silver/Crystal — NAME/ID/"
                       "MONEY/COINS/MOM'S MONEY+SAVE/BADGES/TIME/DEX/RIVAL/MOTHER rows, "
                       "the same blue selection panel as the Gen-3 plain page")

    s.press_n("DOWN", 2)                       # NAME -> ID -> MONEY
    s.tap("A", settle=BIG_SETTLE)               # open num_entry("MONEY", ...)
    s.shot("02_money_entry", "#49-P1: A on MONEY opens the same osk_search-backed "
                              "num_entry(...,999999) the Gen-3 card uses, no clamp message")
    s.tap("SEL", settle=BIG_SETTLE)              # osk_search: SELECT cancels (B deletes a
                                                  # char here, not "back" -- see osk.c's own
                                                  # footer, "A ins B del L/R caret ST ok")

    # DOWN from MONEY: COINS, MOM $, MOM SAVE, BADGES.
    s.press_n("DOWN", 4)
    s.tap("A", settle=BIG_SETTLE)               # open the badge toggle screen
    s.shot("03_badges_before", "#49-P1: BADGES opens the 16-row Johto+Kanto toggle "
                                "screen (trainer_flag_row_paint, same look as Gen 3's "
                                "flag_set_editor)")
    s.tap("A", settle=SETTLE)                   # toggle the first badge (Johto 1)
    s.shot("03b_badges_toggled", "#49-P1: A toggles Johto 1 on/off in the LOCAL "
                                  "GbTrainer copy — nothing is written to the session yet")
    s.tap("B", settle=BIG_SETTLE)               # back to the card

    s.tap("START", settle=BIG_SETTLE)           # a badge was toggled above, so t != t0:
                                                  # START now shows the identity-adjacent
                                                  # "Save trainer changes?" confirm (P1b
                                                  # review D2) instead of writing straight
                                                  # away
    s.shot("04_start_commit", "#49-P1 (D2 recapture): START with a real local edit "
                               "(the badge toggle above) now asks \"Save trainer "
                               "changes? / Writes the card edits now.\" before gbt_write "
                               "ever runs — a no-op START (nothing touched) instead "
                               "returns silently with no popup at all (see 04b)")
    s.tap("A", settle=BIG_SETTLE)               # A = yes -> gbt_write -> gb_persist
    s.shot("04b_start_result", "#49-P1 (D2 recapture): whatever this build's own "
                                "app_can_edit()/sf_write_verified actually do under mGBA "
                                "after confirming (delta build: no real Omega/SD, but the "
                                "delta save IS a real writable file, so this may be a "
                                "genuine commit rather than a refusal — see this shot)")
    return s


def run_red_trainer(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "p1b_red_")
    print("== Red.sav (Gen 1) -- BACKLOG #49 P1b trainer card ==")

    s.run(BIG_SETTLE)                          # #279: no info page -- the grid opens directly; this run() rides out its cold fetch
    s.tap("START", settle=BIG_SETTLE)          # box grid -> the nav menu
    s.press_n("DOWN", 3)                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=BIG_SETTLE)               # Trainer -> pdna_gbtrainer(), card view
    s.shot("01_card", "#49-P1: the Gen-1/2 trainer card, Red/Blue/Yellow — NO mom rows, "
                       "8 badges (not 16), no gender/mother rows (gbtr_build_rows omits "
                       "them for this game)")

    s.press_n("DOWN", 3)                       # NAME -> ID -> MONEY -> COINS
    s.tap("DOWN", settle=SETTLE)                # COINS -> BADGES (no mom rows in between)
    s.tap("A", settle=BIG_SETTLE)               # open the 8-row badge toggle screen
    s.shot("02_badges", "#49-P1: BADGES opens an 8-row toggle screen (Gen 1: one badge "
                         "byte, no Johto/Kanto split)")
    s.tap("B", settle=BIG_SETTLE)               # back to the card — no badge was toggled,
                                                  # so `t` still equals the gbt_read snapshot

    s.tap("START", settle=BIG_SETTLE)           # P1b review D2: a no-op START (t == t0)
                                                  # now returns silently, same as B — no
                                                  # confirm popup, no gbt_write, no backup
                                                  # rotation
    s.shot("03_noop_start", "#49-P1 (D2): START with ZERO local edits returns straight "
                             "to the nav menu — no \"Save trainer changes?\" popup, no "
                             "gbt_write, no backup rotation (memcmp(&t,&t0,..) catches "
                             "it before app_confirm is ever called)")
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, help="pokedna-delta.gba fused with Gold.sav")
    ap.add_argument("--red", type=Path, help="pokedna-delta.gba fused with Red.sav")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "p1b")
    ap.add_argument("--mgba-vendor", type=Path, default=None,
                     help="override the mgba python vendor dir (see load_mgba() above)")
    a = ap.parse_args(argv)

    runs = [("gold", a.gold, (run_gold_trainer,)), ("red", a.red, (run_red_trainer,))]
    active = [(label, p, fns) for label, p, fns in runs if p is not None]
    if not active:
        sys.exit("nothing to do: pass at least one of --gold/--red")
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
