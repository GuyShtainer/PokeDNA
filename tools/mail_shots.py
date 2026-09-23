#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mail_shots.py -- BACKLOG #227's shot chain: Gen 3 refuses to box a party mon that
is holding Mail.

Reuses tools/gb_shots.py's Session/load_mgba (imported, not copied or edited) and the
SAME SWAP recipe tools/party_mail_e2e.py's own docstring documents (box 0 slot 0,
Magnemite, picked up and swapped onto the party overlay's seeded panel slot, index 1 --
the party this save carries is already 6/6, so this is party_place_held's SWAP arm, the
exact site BACKLOG #227 fixed). tools/mail_inject.c is run FIRST (outside this script,
see the shell recipe below) to set slot 1's held item to a Mail id (121, ORANGE MAIL)
through the real gen3_edit.c/gen3_save.c pipeline -- never a hand-poked byte.

THE UI RECIPE (identical prefix to run_b225_party_mail, tools/g3_shots.py, up to the
point where the fix diverges retail behaviour): SEL (cursor mode -> MOVE), A (pick up
cell 0, Magnemite), UP (grid top -> top tabs, PARTY, still carrying), A
(party_strip_overlay PLACE mode) -- then, where the pre-#227 build would let A swap the
carried mon onto slot 1 (Magnemite lands in the party, the mail holder gets boxed with
its mail byte dropped), the FIXED build refuses: A on the seeded panel slot now hits
party_place_held's SWAP arm's g3_party_rec_has_mail(pslot) check BEFORE either half of
the swap runs, and msg_wait shows PDNA_XFER_PARTYFULL_MAIL_TITLE/L1/L2 ("CAN'T DEPOSIT
/ PLEASE REMOVE MAIL / from that Pokemon first.") instead of completing the swap.

Usage (private -- this fixture embeds Guy's own real trainer save, never publish it):
    /tmp/mail_inject /path/to/Emerald.sav 1 121 /tmp/pdna-mail-shot/Emerald_mail.sav
    python3 tools/fuse_sav.py pokedna-delta.gba /tmp/pdna-mail-shot/Emerald_mail.sav \
        -o /tmp/pdna-mail-shot/fused_mail.gba
    /usr/local/bin/python3 tools/mail_shots.py \
        --fused /tmp/pdna-mail-shot/fused_mail.gba --out /tmp/pdna-mail-shot/shots \
        --mgba-vendor /tmp/rec2mp4/vendor
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from gb_shots import Session, load_mgba, BIG_SETTLE   # noqa: E402 (Session helpers only)


def run_b227_party_mail(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #227: the SWAP arm refuses to box a Mail-holding party mon."""
    s = Session(core_mod, image_mod, rom, out_dir, "b227_")
    print("== BACKLOG #227: Gen 3 refuses to box a party member holding Mail ==")

    s.tap("SEL", settle=BIG_SETTLE)        # cursor mode NORMAL -> MOVE
    s.tap("A", settle=BIG_SETTLE)          # pick up cell 0 (Magnemite, species 81) -> carrying
    s.tap("UP", settle=BIG_SETTLE)         # off the grid top -> top tabs, PARTY (still carrying)
    s.tap("A", settle=BIG_SETTLE)          # PARTY tab -> party_strip_overlay, PLACE mode
    s.shot("01_place_overlay", "#227: the party overlay's PLACE mode, having carried "
                                "Magnemite here -- the carried mon itself is not rendered "
                                "in this view; the left card shows the currently SELECTED "
                                "panel slot (Salamence, item ORANGE MAIL, the seeded "
                                "mail-injected mon at party index 1), and the icon grid on "
                                "the right is the box (30/30), unrelated to the carry. A "
                                "here takes party_place_held's SWAP arm, targeting this "
                                "selected slot")

    s.tap("A", settle=BIG_SETTLE)          # attempt the swap onto the mail-holding slot
    s.shot("02_mail_refused", "#227: the SWAP is refused -- msg_wait shows "
                               "PDNA_XFER_PARTYFULL_MAIL_TITLE/L1/L2 (\"CAN'T DEPOSIT / "
                               "PLEASE REMOVE MAIL / from that Pokemon first.\") instead "
                               "of completing the swap",
           claim=["CAN'T DEPOSIT", "PLEASE REMOVE MAIL"])

    s.tap("A", settle=BIG_SETTLE)          # dismiss the refusal
    s.shot("03_still_carrying", "#227: after dismissing the refusal, back in PLACE mode "
                                 "-- still carrying Magnemite (not rendered here, same as "
                                 "frame 01); the left card and the box grid are unchanged "
                                 "from frame 01 (still Salamence / ORANGE MAIL selected), "
                                 "proving the swap never ran and party slot 1 still holds "
                                 "the mail mon")

    s.tap("B", settle=BIG_SETTLE)          # cancel the carry, back to the box grid
    return s


def run_b227_negative_control(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #227 review D4's negative control: the guard must refuse ONLY the
    mail-holding slot, not the whole party overlay -- proven by swapping onto a
    DIFFERENT, ordinary party slot and showing the swap actually goes through (no
    refusal dialog). Same prefix as run_b227_party_mail up to entering PLACE mode
    (the panel opens with the seeded mail-holding slot, index 1, already selected --
    see 01_place_overlay's own caption), then ONE extra DOWN moves the panel focus off
    that slot onto the next real party member (this corpus save's Salamence -> Metagross,
    holding LEFTOVERS, not Mail) before A. Frames re-derived against a live mGBA run,
    not assumed: /tmp/mi-shots-ctrl/ctrl_01_on_nonmail_slot.png / ctrl_02_after_A.png
    are the reviewer's own reference captures of this exact sequence."""
    s = Session(core_mod, image_mod, rom, out_dir, "b227_ctrl_")
    print("== BACKLOG #227 D4: negative control -- swap onto a non-mail slot succeeds ==")

    s.tap("SEL", settle=BIG_SETTLE)        # cursor mode NORMAL -> MOVE
    s.tap("A", settle=BIG_SETTLE)          # pick up cell 0 (Magnemite, species 81) -> carrying
    s.tap("UP", settle=BIG_SETTLE)         # off the grid top -> top tabs, PARTY (still carrying)
    s.tap("A", settle=BIG_SETTLE)          # PARTY tab -> party_strip_overlay, PLACE mode
    s.tap("DOWN", settle=BIG_SETTLE)       # off the seeded mail-holding slot (index 1, Salamence)
                                            # onto the next party member (Metagross, LEFTOVERS)
    s.shot("01_on_nonmail_slot", "#227 D4 negative control: panel focus moved DOWN off "
                                  "the mail-holding slot (index 1, Salamence/ORANGE MAIL) "
                                  "onto the next party member -- the left card now shows "
                                  "Metagross, item LEFTOVERS, not Mail; the carried "
                                  "Magnemite is still not rendered in this view (same as "
                                  "01_place_overlay/03_still_carrying) -- A here must "
                                  "complete the swap, since this slot is not a mail holder")

    s.tap("A", settle=BIG_SETTLE)          # attempt the swap onto the non-mail slot
    s.shot("02_after_A", "#227 D4 negative control: the swap went through -- no CAN'T "
                          "DEPOSIT/PLEASE REMOVE MAIL dialog appeared; the view returned "
                          "straight to the box grid (title/footer changed to the box's "
                          "own MOVE-mode chrome), proving the guard refuses ONLY the "
                          "mail-holding slot, not every party target",
           claim_absent=["CAN'T DEPOSIT", "PLEASE REMOVE MAIL"])
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fused", type=Path, required=True,
                     help="pokedna-delta.gba fused with the mail-injected save "
                          "(tools/mail_inject.c + tools/fuse_sav.py, no --gb)")
    ap.add_argument("--out", type=Path, required=True,
                     help="PRIVATE output dir -- the fused image and its shots embed "
                          "Guy's real trainer save, never write into docs/shots/")
    ap.add_argument("--mgba-vendor", default=None)
    a = ap.parse_args(argv)

    if not a.fused.is_file():
        sys.exit(f"--fused: {a.fused}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    if a.mgba_vendor:
        sys.path.insert(0, a.mgba_vendor)
    core_mod, image_mod = load_mgba()

    sess = run_b227_party_mail(core_mod, image_mod, a.fused, a.out)
    # BACKLOG #227 D4: a FRESH Session (fresh core, same fused image reloaded from its
    # own seed save) -- the negative control needs the emulator back at its start
    # state, not wherever run_b227_party_mail's own B-cancel left it.
    ctrl = run_b227_negative_control(core_mod, image_mod, a.fused, a.out)

    any_failed = False
    for label, s in (("b227", sess), ("b227 D4 negative control", ctrl)):
        print(f"\n{label}: {len(s.taken)} shot(s) taken, {len(s.skipped)} skipped")
        for name, caption, claim_info in s.taken:
            failed = claim_info.get("claim_failed") if claim_info else None
            print(f"  {name}: {'CLAIM FAILED ' + str(failed) if failed else 'ok'}")
        any_failed = any_failed or s.any_claim_failed
    return 1 if any_failed else 0


if __name__ == "__main__":
    sys.exit(main())
