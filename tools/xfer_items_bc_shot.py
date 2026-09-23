#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""xfer_items_bc_shot.py -- BACKLOG #260 (item-shots lane): the three item-ladder
outcomes tools/xfer_items_shot.py's own module docstring left SKIPPED, honestly, not
faked -- see that file's Scene B comment for the wall it hit (box 2's fixture held no
item and was shared by BACKLOG #246's whole existing shot-chain surface). This lane's
own bank_plant.c edit (source/bank_plant.c's bank_plant_g3_box()) cleared that wall:
slot 0 now carries a POTION (Gen-3 id 13, no Secret ID), slot 1 is a SEPARATE genuine
Gen-3-native record with the SAME item AND a nonzero Secret ID -- see that function's
own header comment for why slot 0's own gen12_convert pipeline could never carry a
Secret ID and slot 1 exists as a result. Both existing shot chains against box 2
(tools/b246_g3_to_gb_shot.py, tools/b246_g3_to_gb_vsd.py) were re-run against the
post-edit ROM and diffed frame-by-frame against a scratch pre-edit build in this
lane's own report -- neither chain's own claim= checks moved; the only pixel
differences are the ones this fixture change is FOR (an "Item: POTION" row/label,
box 2's own slot count, and a new "TAKE ITEM" menu row whose LIST POSITION does not
shift MOVE's own index, confirmed by both chains' still-identical downstream frames).

Scene B (case B, "-> bag"): box 2 slot 0 (item, no SID) carried onto Red -- the SAME
corpus save every other GB-session shot chain in this tree uses, whose Items pocket
already had room (confirmed empirically: g3gb_item_ladder's own BAG branch fires,
matching tools/b246_g3_to_gb_vsd.py's own re-run frame 06 finding). The drop is
CONFIRMED (not cancelled, unlike xfer_items_shot.py's Scene A) on a --vsd (real FAT16)
image, mirroring tools/b246_g3_to_gb_vsd.py's own recipe: the sidecar write succeeds
for real, gbs_insert()/gbb_insert_and_write() both run, and the diff (vsd_report())
below is checked for a fresh .pds file as proof the insert actually happened, before
gb_persist()'s own, later PDNA_DELTA-branch wall refuses the CARD write. The brief's
"what the bag looks like afterwards" was ATTEMPTED and is reported NOT STAGED -- see
the comment at that point in main() for what the vehicle actually does instead.

Scene F3 (Secret-ID suffix): the SAME carry, box 2 slot 1 instead of slot 0 -- the
loss screen's item row now carries loss_item_text()'s sid=true branch, the composite
"POTION -> bag + Secret ID" the brief names verbatim. Cancelled with B (not confirmed)
-- one real card-write demo (Scene B) is this lane's own scope; a second serves no new
claim.

Case C ("-> item PC", bag full) needs a THIRD save state (this same corpus Red.sav's
Items pocket filled to its 20-entry cap) and is therefore a SEPARATE fused image and a
separate script: tools/xfer_items_casec_shot.py, which builds that image itself (never
hand-edits the shared corpus save -- see its own module docstring).

    cd <worktree> && /usr/local/bin/python3 tools/xfer_items_bc_shot.py [--out DIR]
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots             # noqa: E402
import dgb_shots            # noqa: E402

ROM = ROOT / "pokedna-delta-gb.gba"

UP_INTO_BANK = 3
UP_TO_ROW0 = 4
DOWN_OFF_BANK = 5


def carry_from_box2(s: gb_shots.Session, slot_right_taps: int) -> None:
    """Bank -> box 2 -> the slot `slot_right_taps` RIGHT-taps from slot 0 -> MOVE ->
    start_carry() -> back on Red's own GB grid. Shared by both scenes below (the SAME
    nav xfer_items_shot.py's own Scene A and tools/b246_g3_to_gb_shot.py both use for
    box 2 itself) so a fixture-nav bug shows up identically in both, not divergently."""
    s.press_n("UP", UP_INTO_BANK, settle=100)
    s.press_n("UP", UP_TO_ROW0, settle=60)
    s.tap("R", settle=150); s.tap("R", settle=150)   # box 0 -> box 1 -> box 2
    for _ in range(slot_right_taps):
        s.tap("RIGHT", settle=80)
    s.tap("A", settle=150)                        # cell menu (native-cell OR plain-cell
                                                    # menu -- either way MOVE sits at
                                                    # index 3, see this lane's own report)
    s.press_n("DOWN", 3, settle=80)                # VIEW/EDIT -> ITEM -> LEGALITY -> MOVE
    s.tap("A", settle=150)                         # MOVE -> start_carry()
    s.press_n("DOWN", DOWN_OFF_BANK, settle=150)   # off the Bank's bottom edge -> GB BOX1


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("/tmp/xfer_items_bc_shot_out"))
    args = ap.parse_args()

    if not ROM.is_file():
        sys.exit(f"missing {ROM} -- run `make delta-gb` in this worktree first")

    core_mod, image_mod = gb_shots.load_mgba()
    args.out.mkdir(exist_ok=True, parents=True)

    # A PLAIN session (no --vsd) has no writable FAT under it at all -- the very FIRST
    # write gb_paste_write() attempts (f_mkdir(APP_DIR)) refuses, showing "SIDECAR
    # FOLDER / Nothing transferred" and returning to the loss/confirm screen for a
    # retry (found live: a script that dismisses that dialog with one more A and
    # keeps tapping A lands right back on the SAME loss screen, byte-for-byte,
    # forever -- gbs_insert()/the bag insert never ran). tools/b246_g3_to_gb_vsd.py's
    # own module docstring documents this exact wall and its fix: a REAL FAT16 image
    # (tools/vsd_img.c) lets the sidecar write succeed for real, so the confirm
    # reaches gbs_insert() + gbb_insert_and_write() (both run) before gb_persist()'s
    # OWN, later PDNA_DELTA-branch refusal -- the SAME two-wall shape that script's
    # own frame 07 demonstrates for the mon-only case; Scene B below is that same
    # demonstration for BACKLOG #248/#249's item insert.
    tmp = Path(tempfile.mkdtemp(prefix="itemshots_vsd_"))
    img = tmp / "sd16.img"
    binp = gb_shots._vsd_img_bin()
    r = subprocess.run([str(binp), "mkimg", str(img), "16"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"mkimg failed: {r.stderr}")
    print(f"vsd image: {img}")

    s = gb_shots.Session(core_mod, image_mod, ROM, args.out, "xferbc_", vsd_img=img)
    print("== item-shots (BACKLOG #260): case B ('-> bag') + F3 (Secret-ID suffix) ==")

    dgb_shots.boot_to_gb_session(s, ROM, which="red")
    s.vsd_snapshot()   # baseline AFTER boot's own log.txt/MIGRATED writes

    # ---- Scene B: box 2 slot 0 (item, no SID) -> a Gen-1 target with bag room -------
    carry_from_box2(s, slot_right_taps=0)
    s.shot("00_carrying_box1_full", "carrying box 2 slot 0 (BULBASAUR + POTION, no "
           "Secret ID) -- back on GB BOX1, 20/20 (no room here)")
    s.press_n("R", 5, settle=150)
    s.shot("01_box_room", "R x5 -> the box with room (19/20, the SAME box tools/"
           "b246_g3_to_gb_vsd.py's own frame 05 finds on this corpus)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("02_loss_screen_bag", "A -- the loss screen's item row: "
           "loss_item_text()'s G3GB_ITEM_BAG/sid=false branch",
           claim="Item: POTION -> bag")
    s.tap("A", settle=gb_shots.BIG_SETTLE)         # A = transfer -- with a real FAT
                                                    # under it, the sidecar write now
                                                    # SUCCEEDS, so gbs_insert() +
                                                    # gbb_insert_and_write() both run
                                                    # for real, THEN gb_persist("paste")
                                                    # hits its OWN, later PDNA_DELTA
                                                    # wall and blocks here
    s.run(300)   # gb_paste_write's own f_mkdir/sf_write_verified/gbs_insert/
                 # gbb_insert_and_write/gb_persist chain draws intermediate panels
                 # before the PDNA_DELTA wall's own msg_wait (b246_g3_to_gb_vsd.py's
                 # own finding: BIG_SETTLE alone was pixel-identical to the loss screen)
    s.shot("03_pdna_delta_wall", "gb_persist()'s own PDNA_DELTA-branch msg_wait -- "
           "the card write is refused on this dev vehicle, but (source/pdna_gen12.c "
           "review D2) the in-session RAM insert from the step above is KEPT for the "
           "rest of this session, which is what the next frame reads back",
           claim=["GAME BOY SAVE", "Edits are in-session only"])
    changed = s.vsd_report()
    pds = [p for p in changed if p.endswith(".pds")]
    print("\n== VSD DIFF (Scene B's own item insert) ==")
    for p in sorted(changed):
        print("   ", p)
    if pds:
        print(f"CONFIRMED (not assumed): the diff DOES contain a .pds file: {pds[0]} "
              "-- the real f_mkdir + sf_write_verified ran, one step before the "
              "compile-time PDNA_DELTA wall would undo it (gb_paste_sidecar_undo has "
              "NOT run yet at this frame -- nothing here has dismissed the msg_wait).")
    else:
        print("NO .pds IN THE DIFF -- the sidecar write did not land; the frames below "
              "are not proof of a real insert. Re-check before trusting them.")
    # ---- "what the bag looks like afterwards" (brief step 2): NOT STAGED ------------
    # Tried, honestly, not faked: dismissing the wall dialog with one more A does NOT
    # return to the GB box grid the way b246_g3_to_gb_shot.py's plain "B = cancel"
    # path does -- found live (a throwaway probe script, not this one): one A after
    # the wall lands back INSIDE the Bank's own box browser (a different Bank cell
    # highlighted), and a second A there picks up and starts carrying THAT cell, not
    # anything on Red's own grid. This vehicle's post-refusal navigation on the
    # PDNA_DELTA wall is therefore NOT the same "back on the grid, carry ended" shape
    # xfer_items_shot.py's Scene A documents for its own B-cancel path -- chasing it
    # further belongs to a BACKLOG line (the wall's own post-dismiss landing screen
    # is undocumented), not to this frame. The .pds diff above is this scene's real
    # evidence that the insert happened; the Bag screen itself is not shot here.
    gb_shots.flush_live_vsd_sessions()

    print()
    print("Scene F3 needs a FRESH session (Scene B's own drop already consumed box 2 "
          "slot 0 and filled the box-with-room's last slot) -- a second Session, not a "
          "second boot within this one.")

    s2 = gb_shots.Session(core_mod, image_mod, ROM, args.out, "xferf3_")
    dgb_shots.boot_to_gb_session(s2, ROM, which="red")
    carry_from_box2(s2, slot_right_taps=1)   # box 2 slot 1: item + nonzero SID
    s2.press_n("R", 5, settle=150)
    s2.shot("00_box_room_slot1", "F3: carrying box 2 slot 1 (item + nonzero Secret "
            "ID) -- R x5 -> the box with room")
    s2.tap("A", settle=gb_shots.BIG_SETTLE)
    s2.shot("01_loss_screen_sid", "A -- loss_item_text()'s sid=true branch: the "
            "composite suffix",
            claim="POTION -> bag + Secret ID")
    s2.tap("B", settle=gb_shots.BIG_SETTLE)        # cancel -- one real card-write demo
                                                    # (Scene B) is this lane's own scope

    total = len(s.taken) + len(s2.taken)
    print(f"\n{total} shot(s) total, {len(s.skipped) + len(s2.skipped)} skip(s)")
    ok = not (s.any_claim_failed or s2.any_claim_failed)
    print(f"{'PASS' if ok else 'FAIL'}: no{'thing' if ok else ' -- SOME'} claim failures")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
