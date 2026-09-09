#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""dgb_shots.py — re-run the GB shot flows on pokedna-delta-gb.gba (BACKLOG #62, #68a).

WHY A SEPARATE SCRIPT (does NOT edit tools/gb_shots.py)
---------------------------------------------------------
tools/gb_shots.py drives PokeDNA's Game Boy fork against a build fused with exactly
ONE GB save (tools/fuse_sav.py --gb) — boot lands straight on S1's info page via
view_save()'s own standalone GB-size fork (memset g_vinfo, pdna_gen12_show_image over
a resident buffer): a full read/write-capable mount (VIEW/EDIT combined, MOVE TO BOX,
RELEASE, CREATE on an empty cell). pokedna-delta-gb.gba (tools/fuse_gb.py, BACKLOG #62)
fuses Emerald.sav (the Gen-3 flash seed) TOGETHER WITH the Red/Gold/Crystal directory.

#68a: view_save() now offers a BOOT PICKER whenever the flash chip holds a valid Gen-3
save AND a GB corpus is fused in (gb_delta_boot_pick(), source/pdna_main.c) — row 0 is
the loaded Gen-3 save, rows 1..n mirror the fused GB saves one for one. Picking the
Gen-3 row continues into the normal Emerald box screen, where the nav menu's "GB
import" row (NV_GB) still offers the READ-ONLY nested mount via
pdna_gen12_show_fused() — VIEW / LEGALITY / COPY / CANCEL, no EDIT, no MOVE TO BOX, no
RELEASE, no CREATE. Picking a GB row instead reuses the SAME full read/write STANDALONE
mount tools/gb_shots.py's own run_gold()/run_red() were calibrated against — VIEW/EDIT,
LEGALITY, MOVE TO BOX, COPY, RELEASE, and a REAL, WORKING CREATE (this build fuses
actual Red.gb/Gold.gbc/Crystal.gbc ROMs alongside the saves, unlike gb_shots.py's
SD-based build, which has no fused ROM and always refuses CREATE). Backing all the way
out of a GB mount (B) reloads the Gen-3 save from flash and re-shows the SAME boot
picker — retired Makefile target `delta-gb-only` used to be the only way to reach this
standalone mount (a second, blank-flash image); the picker makes it reachable from the
one combined image instead, so this script now drives everything off `--image` alone.

#62 review D9's COLD-FETCH COST: the first time ANY generation's sprite/icon art is
fetched this session, gb_art_source.c's PDNA_DELTA half does a from-scratch whole-ROM
scan (no loc-cache entry yet) — measured ~250-260 s of EMULATED GBA time (matches the
D1 review's own "~240 s per cell" estimate) for a 1 MB Gen-1 ROM. Every FOLLOWING fetch
of that same generation reuses the loc cache D1 built and is fast (measured ~7 s of
emulated time, a ~38x speedup). GB_ART_COLD_SETTLE below rides out the first, slow
fetch per generation; ordinary BIG_SETTLE covers everything after it. Getting this
wrong does not fail loudly — it just captures whatever partial-repaint frame mGBA
happens to be on when the settle window ends (a checkerboard placeholder, not real
art), so this constant is deliberately generous, not tuned to the bare minimum.

Usage:
    /usr/local/bin/python3 tools/dgb_shots.py --image pokedna-delta-gb.gba \
        --out docs/shots/gb
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402 -- Session, load_mgba, KEY, HOLD/SETTLE/BIG_SETTLE

# fuse_gb.py preserves command-line order; the Makefile's delta-gb recipe fuses
# Red.sav, Gold.sav, Crystal.sav in that order -> directory SAV-only indices 0, 1, 2
# (fused_gb_save() enumerates SAV entries only, in directory order). The #68a boot
# picker's GB rows use this same order, offset by +1 (row 0 is the Gen-3 save).
PICK_INDEX = {"red": 0, "gold": 1, "crystal": 2}

# source/pdna_layout.h PDNA_NAV_ITEMS: two 10-row columns, Party at (col0, row0).
# NV_GB is column 1, row 6 (Blocks=row0 of col1 .. GB import=row6) -- same arithmetic
# tools/gb_shots.py's own run_e4_settings() uses for NV_SETTINGS (row 7, one further).
NV_GB_DOWN_FROM_COL1_TOP = 6

# #62 review D9: rides out the one-time, per-generation cold ROM scan (see module
# docstring). Gen 1 (a single sprite-portrait scan) measured ~258 s; Gen 2 pays TWO
# independent cold scans the first time a box grid needs BOTH the left-panel portrait
# (gb_art_fetch, its own s_dsprite_loc cache) and the grid's own 16x16 menu icons
# (gb_art_fetch_icon, its own separate s_dicon_loc cache) -- measured ~500 s. 32,000
# frames = ~533 s of emulated GBA time, comfortably over the Gen-2 worst case.
GB_ART_COLD_SETTLE = 32000


def run_nav_gb(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """pokedna-delta-gb.gba: boots into #68a's boot picker (row 0 = Emerald.sav, the
    valid flash save; rows 1..3 = the fused GB corpus) -- A on the default Emerald
    selection continues into the ordinary Gen-3 box screen, from which this drives the
    nav menu to NV_GB, opens the (separate) nested-import picker, picks `which`, and
    captures what the nested-import mount actually shows: the info page, the box grid
    (real Game Boy art -- BACKLOG #62's whole point, cold-fetched the first time), the
    occupied-cell menu, and VIEW (a real portrait via the shared origin-art router)."""
    idx = PICK_INDEX[which]
    # "dgb_" prefix: tools/gb_shots.py's own run_gold()/run_red() already claim
    # gold_01_info.png etc. for the single-fused-save build's standalone S1..S5 flow --
    # this build's shots are a DIFFERENT screen (the nested NV_GB import mount, see
    # module docstring) and must not collide with or overwrite that existing evidence.
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"dgb_{which}_")
    print(f"== delta-gb / {which}: NV_GB import (real art) ==")

    # Session.__init__'s own 180-frame settle is calibrated against a plain single-
    # fused-save image (gb_shots.py's own targets); this recipe's Emerald.sav + the
    # whole GB directory boots slower (calibrated by hand: still on "Opening save...
    # N/13 first paint: box" at 180 frames) -- extra settle before the first tap so
    # A actually lands past the boot picker, not mid-load.
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)              # #68a boot picker, Emerald row (default) -> box
    s.tap("START", settle=gb_shots.BIG_SETTLE)          # box screen -> nav menu
    s.tap("RIGHT")                                        # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", NV_GB_DOWN_FROM_COL1_TOP)           # Blocks -> ... -> GB import
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # NV_GB -> the save picker
    for _ in range(idx):
        s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # picked -> this save's own S1 info page
    s.shot("01_info", f"#62: {which}.sav — S1 info page, opened via NV_GB "
                       "(pdna_gen12_show_fused, read straight out of cartridge space)")

    s.tap("A", settle=60)                                 # info -> box grid (COLD fetch starts)
    s.run(GB_ART_COLD_SETTLE)                             # ride out the first-ever real scan
    s.shot("02_box_grid", f"#62: {which}.sav — box grid with REAL {which.upper()} art: "
                           "sprites decoded straight out of the fused ROM (D1/E3), the same "
                           "16x16 menu icons Gen-2's own PC uses where this save is Gen 2 (E5). "
                           "The portrait is a 4-shade Game Boy sprite, not the full-colour "
                           "Gen-3 stand-in a build without D1's fix would show here.")

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


def run_standalone(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """pokedna-delta-gb.gba (the ONLY image now -- BACKLOG #68a retires the separate
    blank-flash `delta-gb-only` recipe): boot lands on gb_delta_boot_pick()'s BOOT
    PICKER (row 0 = the loaded Emerald.sav, rows 1..3 = Red/Gold/Crystal), because the
    flash chip holds a valid Gen-3 save AND a GB corpus is fused in. Selecting a GB row
    (here: Red, one DOWN from the default Emerald selection) drives the SAME full
    read/write-capable STANDALONE mount tools/gb_shots.py's own run_gold()/run_red()
    drive against an SD-based single-fused-save image, but here backed by a REAL fused
    ROM, so RELEASE and CREATE (which that build can only ever refuse -- "Needs your
    Gen 1/2 ROM", no flashcart in mGBA) genuinely work.

    Uses Guy's own roms/gb corpus, which happens to have EVERY box on EVERY save
    completely full (a "living dex" style test save) -- there is no empty cell to
    CREATE into anywhere without first making one, so this drives RELEASE on one
    occupied cell first (safe: PDNA_DELTA's whole flash chip is a snapshot; gb_persist's
    D2/D5 in-session-only refusal means the release, like the later CREATE, is never
    actually written anywhere) and then CREATEs into the freed slot -- covering both
    D2/D5 (the refusal) and #68a's boot picker + the standalone mount it unlocks, in
    one continuous, honest session. Backing all the way out (B) reloads the Emerald
    save from flash and re-shows the SAME boot picker; picking its Emerald row from
    there continues into the ordinary Gen-3 box screen, where NV_GB's nested import is
    exercised too (see the end of this function) to prove it survived #68a untouched."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "dgb_boot_")
    print("== delta-gb (combined): boot picker, standalone mount, real CREATE, "
          "in-session refusal, then back to Emerald + NV_GB ==")

    # Session.__init__'s own 180-frame settle is calibrated against a plain single-
    # fused-save image; this combined build's Emerald.sav + the whole GB directory
    # boots slower (same reasoning as run_nav_gb's own s.run(700) below).
    s.run(700)
    s.shot("01_boot_picker", "#68a: gb_delta_boot_pick() -- the combined image's flash "
                              "chip holds a valid Emerald.sav AND a GB corpus is fused "
                              "in, so boot now offers this picker instead of going "
                              "straight into the Emerald session: row 0 'Emerald.sav "
                              "(Gen 3)' (default selection) + one row per fused GB save "
                              "(Red/Gold/Crystal)")

    s.tap("DOWN", settle=gb_shots.SETTLE)                 # Emerald (row 0) -> Red (row 1)
    s.tap("A", settle=60)                                 # pick Red -> S1
    s.shot("02_s1_info", "#68a: Red.sav's own S1 info page -- the standalone "
                          "mount, reached via the boot picker's Red row")

    s.tap("A", settle=60)                                 # -> box grid (COLD fetch, gen1 loc empty)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("03_box_grid", "#62 D1: box grid with REAL Red/Blue/Yellow art -- 4-shade Game Boy "
                           "sprites decoded from the fused Red.gb ROM, the first-ever cold scan "
                           "(D9: ~258 s of emulated GBA time) already ridden out")

    # This corpus's every box is full -- RELEASE the box's own first mon (top-left) to
    # open a slot CREATE can use. Menu order: VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY,
    # RELEASE, CANCEL (4 DOWNs from the top to RELEASE).
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # cell menu
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)          # -> RELEASE
    s.shot("04_release_menu", "#62 A3: the occupied-cell menu the STANDALONE mount offers -- "
                               "VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL (the "
                               "nested-import mount's VIEW/LEGALITY/COPY/CANCEL, plus every "
                               "write action, since this session can actually edit)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # "Release this Pokemon?"
    s.shot("05_release_confirm", "#62: the release confirm dialog")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A = yes -> gb_persist() -> PDNA_DELTA refusal
    s.shot("06_refusal", "#62 D2/D5: gb_persist()'s PDNA_DELTA branch -- 'Edits are in-session "
                          "only in the emulator build.' The release is NOT written anywhere "
                          "(there is no SD card, and this build's flash chip stays blank "
                          "either way), but D2's fix means it also is NOT lost: pristine is "
                          "re-baselined to the post-release image right here.")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # dismiss -> back at the box, now 19/20

    # Navigate to the freed cell: this cursor position is this specific corpus's own
    # empty slot after ONE release from the box's top-left mon -- calibrated by hand
    # against Guy's real Red.gb/Red.sav dump, not a general rule about box layout.
    s.tap("UP", settle=gb_shots.SETTLE)
    s.tap("UP", settle=gb_shots.SETTLE)
    s.press_n("LEFT", 6, settle=gb_shots.SETTLE)
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)
    s.shot("07_empty_cell", "#62: cursor on the freshly-released, now-empty cell (19/20)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # empty-cell menu: EMPTY / CREATE / CANCEL
    s.shot("08_create_menu", "#62 A3: the empty-cell menu -- EMPTY (header) / CREATE / CANCEL")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # select CREATE -> pick_species(1)
    s.shot("09_species_picker", "#62: CREATE opens the same pdna_pick.c pick_species() screen "
                                 "Gen-3's own app_create_mon uses -- Bulbasaur (dex 1) "
                                 "pre-selected")

    s.tap("A", settle=60)                                 # pick Bulbasaur -> gb_create_learn (ROM read)
    s.shot("10_busy_line", "#62 A3: 'Reading your ROM... This can take a moment.' -- unlike "
                            "gb_shots.py's SD-based CREATE test (which always refuses here, no "
                            "flashcart in mGBA), THIS build has a real fused Red.gb, so the "
                            "level/moveset lookup (rom_gblearn) genuinely runs")
    s.run(GB_ART_COLD_SETTLE)                             # gb_create_learn's own ROM read
    s.shot("11_new_mon_summary", "#62 A3: the NEW-mon summary -- a real 4-shade Game Boy "
                                  "portrait (decoded from the fused ROM, not a Gen-3 stand-in), "
                                  "Name/Lv/OT/ID/Status fields, EDIT mode (create-mode UX-parity "
                                  "with Gen 3's own create flow)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)            # -> "Keep this Pokemon?"
    s.shot("12_keep_confirm", "#62 D5: 'A = write (session only)' / 'Kept for this session "
                               "only, not written to a card.' -- the gated wording from D5's "
                               "fix, replacing the SD half's 'backs up first' promise this "
                               "build cannot honour")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A = write (session only) -> refusal
    s.shot("13_refusal_2", "#62 D2/D5: the same in-session-only refusal as 06 -- CREATE goes "
                            "through gb_persist() too")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # dismiss -> box grid, new mon showing
    s.shot("14_mon_in_grid", "#62 A3: the box grid re-paged with the newly created Bulbasaur "
                              "showing in the slot RELEASE freed -- CREATE end to end, on real "
                              "fused-ROM art, kept in-session per D2's fix")

    # 3x B: the post-CREATE grid still has a selection/cursor-mode layer or two to
    # unwind (calibrated by hand -- 1x B is enough from a box grid entered fresh, with
    # no create/release history) before pdna_box() itself returns 0 and the session
    # actually leaves.
    s.press_n("B", 3, settle=gb_shots.BIG_SETTLE)
    s.shot("15_back_at_picker", "#68a: backing all the way out of the standalone session (B) "
                                 "reloads the Emerald save from flash (the GB mount had "
                                 "overwritten g_save) and returns straight to "
                                 "gb_delta_boot_pick() -- the same boot picker as shot 01, not a "
                                 "dead end and not a separate blank-flash build")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                # B in the boot picker -> always = Emerald
    s.shot("16_emerald_box", "#68a: KEY_B on the boot picker always resolves to the Emerald row "
                              "-- with a valid flash save on hand there is no dead end to guard "
                              "against, unlike gb_delta_pick_save()'s blank-flash picker (KEY_B "
                              "there exits to nothing but itself). Lands in the ordinary Gen-3 "
                              "box screen, exactly as any delta build without a fused GB corpus "
                              "would boot straight to.")

    # NV_GB nested import, exercised from INSIDE this same Emerald session, to prove it
    # is unaffected by #68a's boot picker: same navigation run_nav_gb() below uses, but
    # inline here so one continuous session covers both the new picker AND the existing
    # nested-import path in one screenshot run.
    s.tap("START", settle=gb_shots.BIG_SETTLE)            # box screen -> nav menu
    s.tap("RIGHT")                                          # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", NV_GB_DOWN_FROM_COL1_TOP)             # Blocks -> ... -> GB import
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # NV_GB -> the (separate) nested-import picker
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # Red (index 0) -> Gold (index 1)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # picked -> Gold's own S1 info page
    s.shot("17_nv_gb_info", "#68a: NV_GB's nested import still works from inside the "
                             "Emerald session after #68a -- Gold.sav's S1 info page via "
                             "pdna_gen12_show_fused(), untouched by the boot-picker change")
    s.tap("A", settle=60)                                   # info -> box grid (COLD fetch, gen2)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the box's own first mon -> its menu
    s.shot("18_nv_gb_mon_menu", "#68a: the nested import's occupied-cell menu is still "
                                 "VIEW / LEGALITY / COPY / CANCEL only -- no EDIT, MOVE TO BOX, "
                                 "or RELEASE (those exist only on the STANDALONE mount reached "
                                 "through the boot picker's GB rows, shots 02-14 above)")
    return s


def _write_manifest(out_dir: Path, ok: list[tuple[str, str]], skipped: list[tuple[str, str]]) -> None:
    """Same merge-by-file/merge-by-name block tools/gb_shots.py's own main() uses
    (BACKLOG #62 review D3: this script never wrote one at all before). Merged, not
    overwritten, for the identical reason: a partial run must not erase captions a
    separate prior run already wrote."""
    manifest_path = out_dir / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    for n, c in ok:
        by_file[n] = {"file": n, "caption": c}
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, required=True,
                     help="pokedna-delta-gb.gba -- the ONE combined image (boot picker, "
                          "standalone GB mount, AND the nested NV_GB import all live here "
                          "since BACKLOG #68a retired the separate delta-gb-only image)")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    a = ap.parse_args(argv)
    if not a.image.is_file():
        sys.exit(f"--image: {a.image}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = gb_shots.load_mgba()

    ok, skipped = [], []
    try:
        sess = run_standalone(core_mod, image_mod, a.image, a.out)
        ok += sess.taken
        skipped += sess.skipped
    except RuntimeError as e:
        print(f"  [STOPPED] boot-picker/standalone: {e}")
    for which in ("gold", "red"):
        try:
            sess = run_nav_gb(core_mod, image_mod, a.image, a.out, which)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] {which}: {e}")

    _write_manifest(a.out, ok, skipped)

    print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
    for name, reason in skipped:
        print(f"  [skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
