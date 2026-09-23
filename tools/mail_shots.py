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
    s.shot("01_place_overlay", "#227: carrying Magnemite into the party overlay's PLACE "
                                "mode -- party is 6/6, seeded panel slot (index 1) holds "
                                "the mail-injected mon (item 121, ORANGE MAIL) -- A here "
                                "takes party_place_held's SWAP arm")

    s.tap("A", settle=BIG_SETTLE)          # attempt the swap onto the mail-holding slot
    s.shot("02_mail_refused", "#227: the SWAP is refused -- msg_wait shows "
                               "PDNA_XFER_PARTYFULL_MAIL_TITLE/L1/L2 (\"CAN'T DEPOSIT / "
                               "PLEASE REMOVE MAIL / from that Pokemon first.\") instead "
                               "of completing the swap",
           claim=["CAN'T DEPOSIT", "PLEASE REMOVE MAIL"])

    s.tap("A", settle=BIG_SETTLE)          # dismiss the refusal
    s.shot("03_still_carrying", "#227: after dismissing the refusal, still in PLACE "
                                 "mode holding Magnemite -- nothing moved (the swap never "
                                 "ran; the party's slot 1 still holds the mail mon)")

    s.tap("B", settle=BIG_SETTLE)          # cancel the carry, back to the box grid
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

    print(f"\n{len(sess.taken)} shot(s) taken, {len(sess.skipped)} skipped")
    for name, caption, claim_info in sess.taken:
        failed = claim_info.get("claim_failed") if claim_info else None
        print(f"  {name}: {'CLAIM FAILED ' + str(failed) if failed else 'ok'}")
    return 1 if sess.any_claim_failed else 0


if __name__ == "__main__":
    sys.exit(main())
