#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""BACKLOG #226 review D4-R(c): the behavioural regression the party-full deposit
rollback (app_party_deposit_undo, source/pdna_main.c) never had. D4-R found that two
mutations of the REAL shipped function that destroy a Pokemon (gutting the append
check with `if (false)`, and D2-R's own zero-before-append bug) both left the host
gate green, because the structural checks in tests/host_escape_gate_sites_test.py
cannot see what happens to the actual Pokemon bytes on a real card write -- only an
emulator boot can. This chain drives the SAME setup on both outcomes of the post-
deposit loss screen (the one refusal the #226 hoist leaves reachable after a deposit
lands, per app_party_full_deposit_offer's own header comment):

  CANCEL (B):  the deposit must roll back -- box header returns to the pre-deposit
               28/30 and GUY (Dragonite, the deposited mon) is back in the party.
  ACCEPT (A):  the deposit must NOT roll back -- box header grows to 29/30 (GUY
               stays deposited) and the incoming held mon lands in the freed slot.

Started from the reviewer's own working recipe (/tmp/rv226_final.py, which this
lane's D3-R/D2-R fixes were re-run against -- see D5-R's re-caption note below) rather
than re-derived from scratch. Box occupancy is asserted MECHANICALLY via claim= (gb_
claims.find against the rendered frame, not eyeballed); the party's OWN member count
cannot be read as a single on-screen digit on this panel (party_strip_overlay shows
sel==0 as an enlarged 'offset tile' separate from the 5-row 'column' list -- 1 + 5 = 6,
not a bug, just an easy miscount from the picture alone -- see the fix brief's own
frame-by-frame table), so the party-restore claim is the presence of the deposited
mon's OWN species text (DRAGONITE / No.149) back in the strip after a cancel.

  cd <lane> && /usr/local/bin/python3 tools/xfer226_deposit_rollback_chain.py . <tag>

Frames land in /tmp/xfer226_<tag>_cancel_out/ and /tmp/xfer226_<tag>_accept_out/.
Needs pokedna-delta-artless-ruby.gba in the lane root (delta-artless fused with a
Ruby.sav from the corpus -- LOCAL USE ONLY, never commit that .gba). Exits 1 if any
claim failed on EITHER path.
"""
import sys, subprocess, tempfile
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
TAG = sys.argv[2] if len(sys.argv) > 2 else "run"
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402

ROM = ROOT / "pokedna-delta-artless-ruby.gba"


def enter_bank(s):
    s.tap("START", settle=80); s.tap("DOWN", settle=60); s.tap("A", settle=150)


def pick_bank_cell(s, right_n):
    enter_bank(s)
    if right_n:
        s.press_n("RIGHT", right_n, settle=80)
    s.tap("A", settle=150); s.tap("DOWN", settle=60); s.tap("A", settle=150)
    s.press_n("DOWN", 5, settle=150)


def carry_to_party_add(s, seed_downs):
    s.tap("UP", settle=150); s.tap("A", settle=200)
    if seed_downs:
        s.press_n("DOWN", seed_downs, settle=120)


def new_session(core_mod, image_mod, out_tag):
    sd = Path(tempfile.mkdtemp(prefix=f"xfer226_{out_tag}_"))
    img = sd / "c.img"
    binp = gb_shots._vsd_img_bin()
    r = subprocess.run([str(binp), "mkimg", str(img), "16"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = Path(f"/tmp/xfer226_{TAG}_{out_tag}_out")
    out.mkdir(exist_ok=True, parents=True)
    return gb_shots.Session(core_mod, image_mod, ROM, out, f"{out_tag}_", vsd_img=img)


def fill_party_to_6_full(s):
    """Boot + two real Bank landings (CHIKORITA, PIKACHU) -> party FULL 6/6, box 28/30
    untouched. Identical on both paths (deterministic input replay)."""
    s.run(700); s.vsd_snapshot()
    s.shot("00_boot", "boot: PC box 1 of Ruby.sav, 28/30 occupied, party 4", claim=["28/30"])
    pick_bank_cell(s, 0); carry_to_party_add(s, 3)
    s.tap("A", settle=300); s.tap("A", settle=400)
    s.shot("01_cell1_landed", "cell1 (CHIKORITA) landed, party 5/6, entry PENDING", allow_same=True)
    s.vsd_snapshot()
    pick_bank_cell(s, 1); carry_to_party_add(s, 4)
    s.tap("A", settle=400); s.tap("A", settle=300); s.tap("A", settle=400); s.tap("A", settle=400)
    s.tap("A", settle=400)
    s.shot("02_party_full", "cell2 (PIKACHU) landed after a real SAVE NOW?; party FULL "
           "6/6, box still 28/30", claim=["28/30"], allow_same=True)
    s.vsd_snapshot()
    pick_bank_cell(s, 3); carry_to_party_add(s, 0)
    # NOTE: no claim=["28/30"] here -- gb_claims does not recognize the outlined SAVE-
    # panel font party_strip_overlay draws its box-occupancy readout in (confirmed by
    # eye, crop+zoom: the pixels DO read "28/30" correctly; this is a claim-checker
    # font gap, not a behavioural defect -- see the fix report's BACKLOG note).
    s.shot("03_strip_full_6", "party strip with a FULL party, box header 28/30 "
           "(verified by eye, not by claim= -- see the checker-gap note above)")
    s.tap("A", settle=400)
    s.shot("04_savenow_first", "the #226/#175c hoist: SAVE NOW? comes first, before the "
           "deposit offer is even shown", claim=["SAVE NOW?"])
    s.tap("A", settle=300); s.tap("A", settle=400); s.tap("A", settle=400)
    s.shot("05_partyfull_offer", "then the #226 PARTY IS FULL deposit offer",
           claim=["PARTY IS FULL", "Send a party Pokemon to a box"])
    s.tap("A", settle=400)
    s.shot("06_picker", "the party picker, 6 real rows", claim=["SEND WHICH PARTY MON?", "GUY"])
    s.tap("A", settle=400)   # pick row 0 = GUY (Dragonite) -> DEPOSIT lands here
    s.shot("07_loss_screen", "GUY (Dragonite) is now deposited into PC box 1 (RAM); the "
           "loss screen is the ONLY post-deposit refusal the hoist leaves reachable",
           claim=["WHAT WON'T TRANSFER"])


def run_cancel(core_mod, image_mod) -> bool:
    s = new_session(core_mod, image_mod, "cancel")
    fill_party_to_6_full(s)
    s.tap("B", settle=500)   # CANCEL -- app_party_deposit_undo must fire
    # claim= only "DRAGONITE" here -- same party_strip_overlay font gap as frame 03;
    # "28/30" is confirmed by eye (crop+zoom) but gb_claims false-negatives on this
    # screen's SAVE-panel font, so asserting it here would make this script flaky for
    # a reason that has nothing to do with the rollback. frame 09 (plain box grid,
    # ordinary font) DOES claim-check 28/30 mechanically, right below.
    s.shot("08_after_cancel_strip", "AFTER B: the strip's offset tile + 5-row column "
           "(1 + 5 = 6) again -- GUY (Dragonite) is back, box header still 28/30 "
           "(verified by eye)", claim=["DRAGONITE"])
    s.tap("B", settle=300)
    s.shot("09_back_on_grid", "B again -- box grid header confirms 28/30, unchanged "
           "from boot", claim=["28/30"])
    print(f"CANCEL path: {len(s.taken)} shots; claim failures: {s.any_claim_failed}")
    return not s.any_claim_failed


def run_accept(core_mod, image_mod) -> bool:
    s = new_session(core_mod, image_mod, "accept")
    fill_party_to_6_full(s)
    s.tap("A", settle=600)   # ACCEPT -- the deposit must NOT be rolled back
    s.shot("08_after_accept_grid", "AFTER A: back on the box grid -- GUY (Dragonite) "
           "stays deposited (header now 29/30, one MORE than boot's 28/30) and the "
           "incoming held mon landed in the freed party slot", claim=["29/30"])
    print(f"ACCEPT path: {len(s.taken)} shots; claim failures: {s.any_claim_failed}")
    return not s.any_claim_failed


def main():
    core_mod, image_mod = gb_shots.load_mgba()
    ok_cancel = run_cancel(core_mod, image_mod)
    ok_accept = run_accept(core_mod, image_mod)
    print(f"\nxfer226_deposit_rollback_chain: cancel={'PASS' if ok_cancel else 'FAIL'} "
          f"accept={'PASS' if ok_accept else 'FAIL'}")
    raise SystemExit(0 if (ok_cancel and ok_accept) else 1)


if __name__ == "__main__":
    main()
