#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""xfer_items_casec_shot.py -- BACKLOG #260 (item-shots lane), case C ("-> item PC"):
the Items pocket is full, so g3gb_item_ladder() (source/gen3_to_gb.c) falls through to
the PC store instead. Needs a THIRD Gen-1 save state on top of Scenes A/B/F3 (tools/
xfer_items_shot.py, tools/xfer_items_bc_shot.py): the shared corpus Red.sav ALREADY has
room in its Items pocket (that is what makes case B reachable at all) -- to reach case
C this script builds its OWN modified Red.sav, once, into a scratch fused image; it
NEVER edits the corpus roms/gb/Red.sav in place (that file is read-only and shared by
every other GB-session shot chain in this tree).

Verified empirically before writing this script (not assumed):
  - roms/gb/Red.sav's own Items pocket already holds 19 of its 20-entry cap
    (GBB_CAP_ITEMS, source/gb_bag.h) -- adding exactly ONE more (a MASTER BALL, id 1,
    chosen only because it is not id 20/POTION -- see below) fills it to 20/20.
  - id 20 (Gen-1 POTION -- item_g3_to_g2(13)==18=="POTION"==Gen-2, then
    item_g2_to_g1(18)==20=="POTION"==Gen-1, source/item_map_g2g3.c/item_map_g1g2.c) is
    NOT already one of those 19 entries: inserting it directly on top of the untouched
    corpus save was refused "pocket is full" rather than merging into an existing
    stack, confirming g3gb_item_ladder's BAG branch is truly unavailable on this
    fixture, not a same-item merge in disguise.
  - the PC store (GBB_POCKET_PC, cap 50) has room on the unmodified corpus save (a
    throwaway insert there succeeded) -- so the ladder's `pc_count < pc_cap` branch is
    reachable and item_outcome lands on G3GB_ITEM_PC, not G3GB_ITEM_STAYS.

Builds tests/host_gbsurgery_tool.c (the tool the brief names for making room) once per
run, applies `--op item items 1 1` to a scratch copy of the corpus Red.sav, and fuses
that copy with Red.gb onto the SAME `pokedna-delta.gba` base `make delta-gb` already
built in this worktree -- a single-fused, no-picker image (tools/dgb_shots.py's own
boot_to_gb_session() handles that shape already; the SAME shape tools/dgb_shots.py's
run_u4_bag()/run_m1_map() use for their own single-ROM images). Nothing this script
writes is committed; the scratch ROM and save live under a tempdir.

    cd <worktree> && /usr/local/bin/python3 tools/xfer_items_casec_shot.py [--out DIR]
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

BASE_ROM = ROOT / "pokedna-delta.gba"
CORPUS_RED_GB = Path.home() / "VSCodeProjects" / "gba-toolkit" / "roms" / "gb" / "Red.gb"
CORPUS_RED_SAV = Path.home() / "VSCodeProjects" / "gba-toolkit" / "roms" / "gb" / "Red.sav"

UP_INTO_BANK = 3
UP_TO_ROW0 = 4
DOWN_OFF_BANK = 5

# tests/host_gbsurgery_tool.c's own header comment: the build line SURGERY_SRCS
# (tools/gb_retail_gate.py) mirrors, kept in the SAME order here.
SURGERY_SRCS = [
    "tests/host_gbsurgery_tool.c", "source/gb_session.c", "source/gb_editor.c",
    "source/gb_edit.c", "source/gen1_save.c", "source/gen1_write.c",
    "source/gen2_save.c", "source/gen2_write.c", "source/data_tables.c",
    "source/rom_gbsprite.c", "source/gb_sprite_codec.c", "source/rom_gbbase.c",
    "source/rom_gblearn.c", "source/gb_new_mon.c",
    "source/gb1_warp.c",
    "source/gb_trainer.c", "source/gb_fields.c",
    "source/gb_bag.c",
    "source/gb_daycare.c", "source/gb_clock.c", "source/gb_fly.c", "source/gb_boxnames.c",
    "source/gb_flags.c", "source/gb_flags_rw.c",
    "source/gb_hof.c",
    "source/gb_dex.c",
    "source/gen3_to_gb.c", "source/gb_moves_legal.c", "source/gen3_mon.c",
    "source/gen3_box.c", "source/evolutions.c", "source/gen3_save.c",
    "source/gen3_edit.c", "source/gen3_daycare.c",
    "source/item_map_g2g3.c", "source/item_map_g1g2.c", "source/gb_item_names.c",
    "source/rom_gbmap.c",
]


def build_surgery_tool(bin_dir: Path) -> Path:
    binp = bin_dir / "hgbsurg"
    cc = "cc"
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT / "source"),
           "-I", str(ROOT / "tests")]
    cmd += [str(ROOT / p) for p in SURGERY_SRCS]
    cmd += ["-o", str(binp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not binp.is_file():
        sys.exit(f"host_gbsurgery_tool build failed:\n{r.stdout}\n{r.stderr}")
    noise = (r.stdout + r.stderr).strip()
    if noise:
        sys.exit(f"host_gbsurgery_tool build produced warnings (must be zero):\n{noise}")
    return binp


def build_fixture(tmp: Path, surgery_bin: Path) -> Path:
    """roms/gb/Red.sav + one MASTER BALL (id 1) -> Items pocket at 20/20 cap, no
    POTION (id 20) among the 20 -- verified by hand before this script existed, see
    the module docstring. Returns the fused, single-ROM, no-picker case-C image."""
    if not CORPUS_RED_SAV.is_file() or not CORPUS_RED_GB.is_file():
        sys.exit(f"missing corpus Red.gb/Red.sav under {CORPUS_RED_SAV.parent}")
    if not BASE_ROM.is_file():
        sys.exit(f"missing {BASE_ROM} -- run `make delta-gb` in this worktree first "
                  "(this script reuses its base image, before the GB ROMs are fused)")

    filled_sav = tmp / "Red_itemsfull.sav"
    r = subprocess.run([str(surgery_bin), "--in", str(CORPUS_RED_SAV),
                         "--out", str(filled_sav), "--op", "item", "items", "1", "1"],
                        capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"filling the Items pocket failed (want exit 0, a 20th entry "
                  f"landing): {r.stdout}{r.stderr}")

    # Sanity check IN THIS SCRIPT, not just in its docstring (golden rule 5 -- assert
    # what the fixture claims): the pocket must now be genuinely full (a further
    # insert of a NEW id is refused), and the target id (20, POTION) must still be
    # ABSENT (else the real chain would merge into an existing stack, which is not
    # case C at all).
    probe = tmp / "Red_itemsfull_probe.sav"
    r2 = subprocess.run([str(surgery_bin), "--in", str(filled_sav), "--out", str(probe),
                          "--op", "item", "items", "2", "1"],
                         capture_output=True, text=True)
    if r2.returncode == 0:
        sys.exit("fixture check FAILED: the Items pocket still had room for a NEW id "
                  "after the fill -- not actually full, case C would not fire")
    r3 = subprocess.run([str(surgery_bin), "--in", str(filled_sav), "--out", str(probe),
                          "--op", "item", "items", "20", "1"],
                         capture_output=True, text=True)
    if r3.returncode == 0:
        sys.exit("fixture check FAILED: id 20 (POTION) was insertable (already "
                  "present as a stack) -- case C needs it ABSENT, not merged")
    print("fixture check: Items pocket is genuinely full (20/20) and id 20 (POTION) "
          "is genuinely absent from it -- case C's own precondition, confirmed live.")

    rom = tmp / "itemshots_casec.gba"
    r4 = subprocess.run([sys.executable, str(ROOT / "tools" / "fuse_gb.py"),
                          str(BASE_ROM), str(CORPUS_RED_GB), str(filled_sav),
                          "-o", str(rom), "--force"],
                         capture_output=True, text=True)
    if r4.returncode != 0:
        sys.exit(f"fuse_gb.py failed:\n{r4.stdout}\n{r4.stderr}")
    return rom


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("/tmp/xfer_items_casec_shot_out"))
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="itemshots_casec_"))
    surgery_bin = build_surgery_tool(tmp)
    rom = build_fixture(tmp, surgery_bin)
    print(f"case-C fixture ROM: {rom}")

    core_mod, image_mod = gb_shots.load_mgba()
    args.out.mkdir(exist_ok=True, parents=True)
    s = gb_shots.Session(core_mod, image_mod, rom, args.out, "casec_")
    print("== item-shots (BACKLOG #260): case C ('-> item PC', bag full) ==")

    dgb_shots.boot_to_gb_session(s, rom, which="red")

    s.press_n("UP", UP_INTO_BANK, settle=100)
    s.press_n("UP", UP_TO_ROW0, settle=60)
    s.tap("R", settle=150); s.tap("R", settle=150)   # box 0 -> box 1 -> box 2
    s.shot("00_box2", "box 2, slot 0 -- the SAME item fixture as cases B/F3, on THIS "
           "save's own full Items pocket", claim=["BANK 3", "2/30"])
    s.tap("A", settle=150)
    s.press_n("DOWN", 3, settle=80)   # VIEW/EDIT -> ITEM -> LEGALITY -> MOVE
    s.tap("A", settle=150)            # MOVE -> start_carry()
    s.press_n("DOWN", DOWN_OFF_BANK, settle=150)
    s.shot("01_carrying_box1", "carrying, back on GB BOX1 -- corpus box fullness is "
           "unaffected by this fixture's own bag edit, so this is 20/20 as on every "
           "other chain against this corpus", claim=["GB BOX1", "20/20"])
    s.press_n("R", 5, settle=150)
    s.shot("02_box_room", "R x5 -> the box with room (GB BOX6, 16/20)",
           claim=["GB BOX6", "16/20"])
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_loss_screen_pc", "A -- the loss screen's item row: "
           "loss_item_text()'s G3GB_ITEM_PC/sid=false branch, reached because "
           "g3gb_item_ladder()'s own items_count>=items_cap test is true on THIS "
           "save and the PC store still has room",
           claim="Item: POTION -> item PC")
    s.tap("B", settle=gb_shots.BIG_SETTLE)   # cancel -- this is a scratch fixture,
                                              # never reused, but B is still the
                                              # cheaper, better-understood path (see
                                              # xfer_items_bc_shot.py's own finding
                                              # about what A's PDNA_DELTA wall does)

    print(f"\n{len(s.taken)} shot(s), {len(s.skipped)} skip(s)")
    ok = not s.any_claim_failed
    print(f"{'PASS' if ok else 'FAIL'}: no{'thing' if ok else ' -- SOME'} claim failures")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
