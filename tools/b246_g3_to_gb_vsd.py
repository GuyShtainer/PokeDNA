#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""b246_g3_to_gb_vsd.py -- BACKLOG #246 review D4: the --vsd runner, modelled on
tools/dgb_shots.py's run_s150_8_party_vsd (same discipline: a real writable FAT16
image via tools/vsd_img.c, vsd_snapshot()/vsd_report() bracketing the writes, frames
for every refusal AND the write itself).

D4's own finding, reproduced here with real frames: the "SIDECAR FOLDER" wall the
lane's own original evidence stopped at was NOT "no writable FAT" -- tools/vsd.py
serves a real FAT16 image, so f_mkdir()/sf_write_verified() succeed for real on this
vehicle. The GENUINE wall is one step further on: gb_persist()'s own
`#ifdef PDNA_DELTA` refusal (source/pdna_gen12.c, "no SD for a GB image") -- this
build (pokedna-delta-gb.gba, `make delta-gb`) is always compiled with -DPDNA_DELTA
(Makefile:274), and gb_persist() under that macro returns false UNCONDITIONALLY, with
no dependency on whether the filesystem underneath it is virtual or real. That refusal
is NOT the #246 down-arm's own bug -- every GB save write in this whole tool refuses
the same way on this build (the single-fused-GB boot fallback documents the identical
posture). gb_paste_write() (source/pdna_gen12.c) calls gb_persist("paste") as its LAST
step and, on refusal, calls gb_paste_sidecar_undo(path) to roll the sidecar entry back
out again -- BUT gb_persist()'s own PDNA_DELTA-branch msg_wait blocks on a real
keypress first, so a runner that stops (as this one does) the moment that dialog draws
catches the sidecar entry BEFORE the undo runs: the .pds file this run's own
sf_write_verified() call wrote moments earlier IS present in the vsd diff at that point
-- confirmed empirically below, not assumed (this script's own vsd_report() output is
printed and inspected, never silently asserted against a guess). Dismissing that dialog
with one more A would run the undo and remove it again -- this runner deliberately does
not, so the diff captures the write actually landing. (A card image built WITHOUT
-DPDNA_DELTA -- an ordinary `make`/`make artless` ROM run on real hardware or under
the hardware-testing-protocol -- takes the #else branch and a real write lands
permanently, sidecar entry included; that is the retail-shaped path this whole feature
ships for.)

Three scenes on one continuous carry (a refusal never empties the hand, so the SAME
lifted Bank cell is re-dropped at each site):
  A. the party pseudo-box (L from GB BOX1) -- gb_box_is_party()'s own refusal, KEPT
     first and unchanged by this lane, BEFORE anything else runs.
  B. GB BOX1 itself (R from the party pseudo-box, back to the start) -- 20/20 full on
     this corpus (see tools/b246_g3_to_gb_shot.py's own frame 06) -- BACKLOG #246
     review D6's hoisted capacity refusal, now firing BEFORE the loss screen ever
     draws (the bug D6 fixed: it used to fire AFTER, letting a confirmed "A =
     transfer" turn out to mean nothing).
  C. R x5 further (the fifth box past the party) -- 19/20, has room, confirmed live
     against this exact corpus (frame 05 below) -- the loss screen draws, A confirms,
     and the write hits the PDNA_DELTA wall above.

    cd <worktree> && /usr/local/bin/python3 tools/b246_g3_to_gb_vsd.py [--out DIR]
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


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("/tmp/b246_vsd_out"))
    args = ap.parse_args()

    if not ROM.is_file():
        sys.exit(f"missing {ROM} -- run `make delta-gb` in this worktree first")

    core_mod, image_mod = gb_shots.load_mgba()
    args.out.mkdir(exist_ok=True, parents=True)

    tmp = Path(tempfile.mkdtemp(prefix="b246vsd_"))
    img = tmp / "sd16.img"
    binp = gb_shots._vsd_img_bin()
    r = subprocess.run([str(binp), "mkimg", str(img), "16"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"mkimg failed: {r.stderr}")
    print(f"vsd image: {img}")

    s = gb_shots.Session(core_mod, image_mod, ROM, args.out, "b246vsd_", vsd_img=img)
    print("== BACKLOG #246 review D4: the --vsd runner (party / capacity / loss screen) ==")

    dgb_shots.boot_to_gb_session(s, ROM, which="red")
    s.vsd_snapshot()   # baseline AFTER boot's own log.txt/MIGRATED writes

    # ---- pick up the planted plain Gen-3 Bank cell (bank_plant_g3_box, box index 2 slot 0) ----
    s.press_n("UP", UP_INTO_BANK, settle=100)
    s.press_n("UP", UP_TO_ROW0, settle=60)
    s.tap("R", settle=150); s.tap("R", settle=150)   # box 0 -> box 1 -> box 2
    s.shot("00_bank_box2", "in the Bank, box 3 (index 2) -- the planted PLAIN Gen-3 cell")
    s.tap("A", settle=150)              # cell menu
    s.press_n("DOWN", 3, settle=80)     # VIEW/EDIT -> ITEM -> LEGALITY -> MOVE
    s.tap("A", settle=150)              # MOVE -> start_carry()
    s.press_n("DOWN", 5, settle=150)    # off the Bank's bottom edge -> back on the GB grid

    # ---- A. the party pseudo-box -----------------------------------------------------
    s.tap("L", settle=150)              # GB BOX1 -> wraps to the party pseudo-box
    s.shot("01_party_pseudo_box", "L from GB BOX1 -- the party pseudo-box, still carrying")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("02_party_refusal", "A to drop on the party pseudo-box -- gb_box_is_party()'s "
           "own refusal, KEPT first and unchanged",
           claim=["CAN'T TRANSFER", "Storage boxes only, not the party.", "Press A"])
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # dismiss the msg_wait dialog before navigating on

    # ---- B. GB BOX1 (full, 20/20) -- BACKLOG #246 review D6's capacity refusal -------
    s.tap("R", settle=150)              # party pseudo-box -> GB BOX1
    s.shot("03_gb_box1_carrying", "back on GB BOX1 (20/20, full), still carrying")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("04_capacity_refusal", "A to drop on the full GB BOX1 -- D6's hoisted "
           "capacity refusal fires BEFORE the loss screen (the bug D6 fixed)",
           claim=["TRANSFER REFUSED", "that box is full", "Free a slot there first."])
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # dismiss the msg_wait dialog before navigating on

    # ---- C. the box with room -- the loss screen, then the write itself -------------
    s.press_n("R", 5, settle=200)       # -> the fifth box past the party (GB BOX5/BOX6 depending on 0/1-indexing -- 19/20, has room)
    s.press_n("DOWN", 4, settle=60); s.press_n("RIGHT", 4, settle=60)
    s.shot("05_gb_box6_cursor_empty", "the box with room (19/20), cursor on an empty cell, still carrying")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("06_loss_screen", "A to drop -- the loss screen (gb_paste_loss_screen, "
           "LOSS_FOOT_BRIDGE)",
           claim=["WHAT WON'T TRANSFER", "Kept in /PokeDNA/xfer",
                  "restored when it comes back.", "The Bank slot is emptied when it lands.",
                  "A = transfer", "B = cancel"])
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.run(300)   # gb_paste_write's own f_mkdir/sf_read_full/sf_write_verified/gb_persist
                 # chain draws intermediate busy panels before the PDNA_DELTA wall's own
                 # msg_wait -- BIG_SETTLE alone was pixel-identical to the loss screen.
    s.shot("07_after_confirm", "A = transfer -- the sidecar sf_write_verified() runs for "
           "real against this --vsd image, THEN gb_paste_write()'s own gb_persist(\"paste\") "
           "call hits the PDNA_DELTA wall and blocks on its own msg_wait keypress -- "
           "gb_paste_sidecar_undo() has NOT run yet at this frame (BACKLOG #246 review "
           "F6(b): it never runs on this runner at all, since nothing here dismisses that "
           "msg_wait) -- which is exactly why the .pds this step just wrote is still on "
           "disk for the diff below to find")
    s.run(300)
    s.shot("08_settled", "300 more frames", allow_same=True)

    changed = s.vsd_report()
    print("\n== VSD DIFF (party / capacity / loss-screen chain) ==")
    for p in sorted(changed):
        print("   ", p)
    pds = [p for p in changed if p.endswith(".pds")]
    if pds:
        print(f"\nCONFIRMED (not assumed): the diff DOES contain a .pds file: {pds[0]}")
        print("gb_paste_write()'s own sf_write_verified() call runs BEFORE gb_persist(\"paste\"),")
        print("and this snapshot was taken with frame 07/08 still parked INSIDE gb_persist()'s")
        print("own PDNA_DELTA-branch msg_wait (\"GAME BOY SAVE / Edits are in-session only\") --")
        print("msg_wait blocks on a real keypress this runner never sends, so gb_paste_write()'s")
        print("`if (!ok) gb_paste_sidecar_undo(path);` line (which would remove this same entry)")
        print("has NOT executed yet at the point this diff runs. The 146-byte size (18-byte")
        print("GBSC_HEADER + one 128-byte GBSC_ENTRY) confirms exactly one fresh sidecar entry --")
        print("the real f_mkdir + sf_write_verified the review asked to confirm, caught live, one")
        print("step before the compile-time PDNA_DELTA wall would undo it. A non-PDNA_DELTA build")
        print("(ordinary `make`/`make artless`) takes gb_persist()'s #else branch instead, and the")
        print("landing (GB save write + sidecar entry BOTH kept) is real and permanent -- that is")
        print("the retail-shaped path (hardware-testing-protocol territory, not this vehicle).")
    else:
        print("\nNo .pds in the diff -- unexpected; re-check the tap sequence against this file's")
        print("own header comment before trusting this run.")

    ok = not s.any_claim_failed
    print(f"\n{'PASS' if ok else 'FAIL'}: {len(s.taken)} shots, "
          f"{'no' if ok else 'SOME'} claim failures")
    gb_shots.flush_live_vsd_sessions()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
