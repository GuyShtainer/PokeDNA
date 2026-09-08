#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""dgb_shots.py — re-run the GB shot flows on pokedna-delta-gb.gba (BACKLOG #62).

WHY A SEPARATE SCRIPT (does NOT edit tools/gb_shots.py)
---------------------------------------------------------
tools/gb_shots.py drives PokeDNA's Game Boy fork against a build fused with exactly
ONE GB save (tools/fuse_sav.py --gb) — boot lands straight on S1's info page via
view_save()'s own standalone GB-size fork (memset g_vinfo, pdna_gen12_show_image over
a resident buffer): a full read/write-capable mount (VIEW/EDIT combined, MOVE TO BOX,
RELEASE, CREATE on an empty cell). pokedna-delta-gb.gba (tools/fuse_gb.py, BACKLOG #62)
fuses Emerald.sav (the Gen-3 flash seed) TOGETHER WITH the Red/Gold/Crystal directory —
and view_save()'s fallback chain checks the single-slot fused Gen-3 save FIRST, which
always wins when it parses. So THIS build's boot lands in the normal Emerald Gen-3 box
screen, and the only reachable path to a fused GB save is the nav menu's "GB import"
row (NV_GB, source/pdna_main.c commit f6731cf), which mounts read-only via
pdna_gen12_show_fused() — the SAME "import a mon from another cart" mount the SD path
already had (pdna_gen12_show), not the full standalone session tools/gb_shots.py's
run_gold()/run_red() were calibrated against. Confirmed by screenshot: the nested
import's occupied-cell menu is VIEW / LEGALITY / COPY / CANCEL — no EDIT, no MOVE TO
BOX, no RELEASE, no CREATE on an empty cell (those only exist on the STANDALONE
mount) — so reusing run_gold()/run_red() past their own early screens captured the
WRONG thing (Emerald's own party screen) under a GB caption. This script therefore
does NOT try to reuse gb_shots.py's later screens or its CREATE drivers at all; it
captures only what the nested-import mount actually offers, correctly captioned.

A delta-gb image with NO Gen-3 save fused (drop the Emerald.sav step) would boot
straight into the standalone mount and let tools/gb_shots.py's own script run
completely unmodified for the full S1-S5 flow incl. CREATE — out of scope for this
pass (the brief's own default recipe fuses both together); left as a follow-up.

Usage:
    /usr/local/bin/python3 tools/dgb_shots.py --image pokedna-delta-gb.gba \
        --out docs/shots/gb
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402 -- Session, load_mgba, KEY, HOLD/SETTLE/BIG_SETTLE

# fuse_gb.py preserves command-line order; the Makefile's delta-gb recipe fuses
# Red.sav, Gold.sav, Crystal.sav in that order -> directory SAV-only indices 0, 1, 2
# (fused_gb_save() enumerates SAV entries only, in directory order).
PICK_INDEX = {"red": 0, "gold": 1, "crystal": 2}

# source/pdna_layout.h PDNA_NAV_ITEMS: two 10-row columns, Party at (col0, row0).
# NV_GB is column 1, row 6 (Blocks=row0 of col1 .. GB import=row6) -- same arithmetic
# tools/gb_shots.py's own run_e4_settings() uses for NV_SETTINGS (row 7, one further).
NV_GB_DOWN_FROM_COL1_TOP = 6


def run_nav_gb(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """Boots into the Emerald Gen-3 box screen (this recipe's real boot state), opens
    the nav menu, drives to NV_GB, opens the save picker, picks `which`, and captures
    what the nested-import mount actually shows: the info page, the box grid (real
    Game Boy art + BACKLOG #62's whole point), the occupied-cell menu, and VIEW (a
    real portrait via the shared origin-art router)."""
    idx = PICK_INDEX[which]
    # "dgb_" prefix: tools/gb_shots.py's own run_gold()/run_red() already claim
    # gold_01_info.png etc. for the single-fused-save build's standalone S1..S5 flow --
    # this build's shots are a DIFFERENT screen (the nested NV_GB import mount, see
    # module docstring) and must not collide with or overwrite that existing evidence.
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"dgb_{which}_")
    print(f"== delta-gb / {which}: NV_GB import (real art) ==")

    s.tap("START", settle=gb_shots.BIG_SETTLE)          # box screen -> nav menu
    s.tap("RIGHT")                                        # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", NV_GB_DOWN_FROM_COL1_TOP)           # Blocks -> ... -> GB import
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # NV_GB -> the save picker
    for _ in range(idx):
        s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # picked -> this save's own S1 info page
    s.shot("01_info", f"#62: {which}.sav — S1 info page, opened via NV_GB "
                       "(pdna_gen12_show_fused, read straight out of cartridge space)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # info -> box grid
    s.shot("02_box_grid", f"#62: {which}.sav — box grid with REAL {which.upper()} art: "
                           "sprites decoded straight out of the fused ROM (E3), the same "
                           "16x16 menu icons Gen-2's own PC uses where this save is Gen 2 (E5)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A on the box's own first mon -> its menu
    s.shot("03_mon_menu", f"#62: {which}.sav — the occupied-cell menu this NESTED IMPORT mount "
                           "offers: VIEW / LEGALITY / COPY / CANCEL only -- no EDIT, MOVE TO BOX, "
                           "or RELEASE (those exist on the STANDALONE mount view_save()'s own GB-"
                           "size fork uses, which this recipe's fused Emerald.sav makes "
                           "unreachable at boot -- see this file's own module docstring)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # VIEW -> the native summary (read-only)
    s.shot("04_view_summary", f"#62: {which}.sav — VIEW: the native read-only summary with a "
                               "REAL portrait (pdna_origin_art's GB router now reads the fused "
                               "ROM instead of falling back to Gen-3 stand-in art)")
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, required=True, help="pokedna-delta-gb.gba")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    a = ap.parse_args(argv)
    if not a.image.is_file():
        sys.exit(f"--image: {a.image}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = gb_shots.load_mgba()

    ok, skipped = [], []
    for which in ("gold", "red"):
        try:
            sess = run_nav_gb(core_mod, image_mod, a.image, a.out, which)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] {which}: {e}")

    print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
    for name, reason in skipped:
        print(f"  [skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
