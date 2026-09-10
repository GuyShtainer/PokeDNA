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

#68a: view_save() now offers a BOOT PICKER whenever a Gen-3 save is READY -- the flash
chip parses, or (the normal case on a fresh emulator boot, where the flash is blank) the
single-slot fused .sav fallback supplies it -- AND a GB corpus is fused in (gb_delta_boot_pick(), source/pdna_main.c) — row 0 is
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


def run_gbscreen_shell(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """U2a (docs/GB-GAME-SCREENS-DESIGN.md sec 3.3): the shared GB-screen shell's own
    standalone demo (source/pdna_gbscreen.c gbscr_run_demo()) -- the located font's
    128-glyph sheet inside a text-box border, reachable via a hidden SELECT key on
    the Settings list (no card exists yet; that is U2b). This build boots into
    #68a's own boot picker first (see run_nav_gb()'s own module-docstring note --
    row 0 is the Emerald save, rows 1..n the fused GB corpus); A on the default
    Emerald selection continues into the normal Gen-3 box screen, from which
    navigation up to the Settings list is IDENTICAL to run_e4_settings()'s own
    (tools/gb_shots.py): START -> nav menu -> RIGHT (col 1) -> DOWN x7 -> A.

    gbscr_open() under PDNA_DELTA does a full, uncached rom_gbui scan every time
    (this slice's EWRAM budget forbids a new EWRAM loc-cache the way #62 D1 gave
    gb_art_source.c's own delta half) -- ridden out with the same GB_ART_COLD_SETTLE
    window dgb_shots.py's own run_nav_gb()/run_standalone() already use for the
    analogous rom_gbsprite cold scan. Only ONE scan per demo ENTRY: gbscr_run_demo()
    opens once and loops on SELECT/A/B without re-opening."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "u2a_")
    print("== U2a: the GB-screen shell demo (Settings > SELECT) ==")

    # Same extra settle run_nav_gb() uses for this SAME image (Emerald.sav + the
    # whole GB directory boots slower than a plain single-fused-save image --
    # Session.__init__'s own 180-frame settle is not enough; without this, START
    # fires mid-load and lands somewhere other than the box screen's own menu).
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)        # #68a boot picker, Emerald row (default) -> box
    s.tap("START", settle=gb_shots.BIG_SETTLE)   # box screen -> nav menu
    s.tap("RIGHT")                                 # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", 7)                           # Blocks -> ... -> Settings (index 17)
    s.tap("A", settle=gb_shots.BIG_SETTLE)         # nav menu -> the Settings list

    s.tap("SEL", settle=GB_ART_COLD_SETTLE)        # hidden key -> gbscr_run_demo(PDNA_GEN1)
    s.shot("01_1to1", "U2a: the GB-screen shell demo -- Red.gb's own located font, "
                       "all 128 glyphs (charmap 0x80..0xFF), inside a border of "
                       "RomGbUi.textbox tile 0, 1:1 centred at (40,8) with the "
                       "legend in the side bars (default mode, gb_scale_mode=0). "
                       "D9: tile 0 of pokered's TextBoxGraphics "
                       "(gfx/font/font_extra.2bpp) renders as a bold 'A', not a "
                       "dialogue-box frame -- the actual frame pieces are tiles "
                       "~24-31, and painting a proper frame is U2b's job, not "
                       "this demo's (it only proves the shell fetches and blits "
                       "a non-font ROM block).")

    s.tap("SEL", settle=60)                        # toggle -> stretched
    s.shot("02_stretched", "U2a: SELECT toggles to stretched (240x160, "
                            "blit_stretched's own y_dst0/y_dst_count tables + "
                            "x's period-2 shift/mask formula -- x duplicates "
                            "every 2nd source column, y every 9th source row) "
                            "-- same font, same tile-0 'A' border (D9, see "
                            "01_1to1's caption), filling the whole screen; the "
                            "bottom scrim now shows the shell's own keys "
                            "'A OK  B BACK  SEL SIZE' plus this screen's 'EXIT' "
                            "beside them (D3 fix -- stretched mode used to "
                            "replace the shell's keys with the screen's own)")

    s.tap("SEL", settle=60)                        # toggle back -> 1:1
    s.shot("03_back_1to1", "U2a: SELECT again returns to 1:1 -- a live toggle, not "
                            "a one-way switch")

    s.tap("B", settle=gb_shots.BIG_SETTLE)          # leave the demo -> Settings list
    s.shot("04_settings_after", "U2a: back at the Settings list -- the demo's "
                                 "content is fully gone (pdna_settings() forces a "
                                 "full repaint on return), proving the shell does "
                                 "not leave stray pixels behind on exit")
    return s


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
    PICKER (row 0 = the loaded Gen-3 save, rows 1..3 = Red/Gold/Crystal), because a
    Gen-3 save is ready (on a fresh emulator boot the flash is blank, so it comes from
    the fused .sav fallback) AND a GB corpus is fused in. Selecting a GB row
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
    s.shot("01_boot_picker", "#68a: gb_delta_boot_pick() -- the combined image has a Gen-3 "
                              "save ready (the fused .sav fallback; the flash is blank on a "
                              "fresh boot) AND a GB corpus fused in, so boot offers this picker instead of going "
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
                                 "reloads the Emerald save from whichever source supplied it (the fused .sav here; the GB mount had "
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


def run_u2c_trainer(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """U2c (docs/GB-GAME-SCREENS-DESIGN.md sec 1.1): Red's OWN trainer card on the
    shared GB-screen shell, over the STANDALONE mount (boot picker DOWN -> Red row ->
    A -> S1 info -- same path run_standalone()'s own shots 01-02 use), then A -> box
    grid (rides out rom_gbsprite's cold scan the same way run_standalone()'s own
    03_box_grid does), START -> nav menu, DOWN x3 -> Trainer -> A ->
    pdna_gbtrainer() -- Gen 1, so this lands on pdna_gbtrainer_gen1_card(), NOT the
    Emerald-art path tools/p1c_shots.py's run_gold_card() shoots (that one is Gen-2-
    only as of this slice). gbscr_open()'s own rom_gbui scan is a SEPARATE cold scan
    from rom_gbsprite's box-grid one (different locator, different ROM regions), so
    entering the card the first time gets its own GB_ART_COLD_SETTLE ride-out."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "u2c_")
    print("== U2c: Red's own trainer card (Red.sav, boot picker -> standalone -> Trainer) ==")

    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)                  # Emerald (row 0) -> Red (row 1)
    s.tap("A", settle=60)                                  # pick Red -> S1 info
    s.tap("A", settle=60)                                  # -> box grid (rom_gbsprite cold fetch)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 3)                                     # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Trainer -> pdna_gbtrainer_gen1_card()
                                                              # (gbscr_open's OWN cold rom_gbui scan)
    s.shot("01_card_1to1", "U2c: Red's OWN trainer card, 1:1 centred at (40,8) -- "
                            "NAME/MONEY/TIME painted with the CARDFRAME block's frame "
                            "tiles for the panel border, the player pic (gb_sprite_gen1, "
                            "4 tile columns x 6 rows -- the real game's own text-box "
                            "border overwrites pic column 5 and row 7, D1 review fix), "
                            "and both 8-badge rows -- all shown OWNED (badge tiles, not "
                            "face tiles): Red.sav's own wObtainedBadges is 0xFF on this "
                            "save, all 8 badges genuinely owned")

    s.tap("SEL", settle=60)                                 # shell-wide toggle -> stretched
    s.shot("02_card_stretched", "U2c: SELECT stretches the SAME card to 240x160 -- "
                                 "the shell's own scale toggle (gb_scale_mode), not a "
                                 "card-specific key; the legend overlays the bottom row")

    s.tap("SEL", settle=60)                                 # back to 1:1
    s.shot("03_card_1to1_again", "U2c: SELECT again returns to 1:1 -- a live toggle")

    # Cursor cycle order (G1C_*): NAME(0) MONEY(1) TIME(2) then the 8 badges row-major.
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("04_cursor_money", "U2c: DOWN moves the cursor from NAME to MONEY")

    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_cursor_time", "U2c: DOWN again -> TIME")

    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("06_cursor_badge0", "U2c: DOWN again -> the first badge (row 1, col 0)")

    # D10 (review): Red.sav's badge0 starts OWNED (see shot 01's own caption), so A's
    # FIRST toggle here goes owned -> UNOWNED (face tile), not the other way around --
    # shots 07/08 were captioned backwards.
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # instant toggle -> UNOWNED
    s.shot("07_badge0_unowned", "U2c: A instantly toggles badge 0 to UNOWNED (it "
                                 "started owned, Red.sav's own wObtainedBadges = 0xFF) "
                                 "-- badge tile swaps to a face tile (never a palette "
                                 "change, design's own 'lit vs unlit is not a palette "
                                 "change' rule)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # toggle back -> OWNED
    s.shot("08_badge0_owned", "U2c: A again toggles it back to OWNED (badge tile)")

    s.press_n("UP", 3, settle=gb_shots.SETTLE)               # badge0 -> TIME -> MONEY -> NAME
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # A on NAME -> the identity WARNING first
    s.shot("09_editor_name", "U2c: A on NAME does NOT open the keyboard directly -- "
                              "gbtr_id_edit_ok() shows the identity-change WARNING "
                              "first ('Changes your TRAINER identity / Your own "
                              "Pokemon become traded', A=yes B=no), once per visit; "
                              "see shot 09b for the actual keyboard")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # confirm the warning -> osk_input opens
    s.shot("09b_editor_name_keyboard", "U2c: A (yes) on the warning -- NOW the "
                                        "EXISTING osk_input() pop-up opens (via "
                                        "gbtr_edit_row, the same sub-editor the plain "
                                        "page and Gen 2's card already share) -- "
                                        "PokeDNA's own on-screen keyboard, not a "
                                        "game-art dialogue")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # cancel -> back on the card, NAME selected
    s.shot("10_back_on_card", "U2c: B cancels the name editor, back on the card")

    # A REAL edit (badge0 toggled, LEFT changed) so B's commit prompt actually fires
    # -- stays in this SAME visit (never exits the card) rather than re-entering the
    # nav menu a second time, which left the game in an unexpected state (the nav
    # menu's own remembered row is not "Trainer" the way a fresh A-press assumes --
    # an earlier version of this script re-entered and landed on a mon's own
    # VIEW/EDIT menu instead; caught by looking at the screenshot, not by the
    # harness's own pixel-diff check, which only flags an IDENTICAL pair).
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)               # NAME -> MONEY -> TIME -> badge0
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # toggle badge0 -- a REAL change
    s.shot("11_real_edit", "U2c: badge0 toggled and LEFT changed (unlike shots "
                            "06-08's toggle-and-back, this one stays changed)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # B -> the actual commit prompt
    s.shot("12_commit_prompt", "U2c: B with a real pending edit -> "
                                "'Save trainer changes?' (app_confirm), the SAME "
                                "dialog Gen 3's own card_editor uses -- shot 10's "
                                "own B (nothing changed, t == t0) instead took the "
                                "silent memcmp no-op path straight back to the "
                                "card, which is why this shot needed a real edit")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # decline -- discard the real edit too
    s.shot("13_declined", "U2c: declining a REAL edit also discards it -- the "
                           "save was never written (gbt_write never ran), back "
                           "at the box grid")
    return s



def run_u3_trainer(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """U3 (BACKLOG #66, docs/GB-GAME-SCREENS-DESIGN.md sec 1.2): Gold/Silver/Crystal's
    OWN trainer card on the shared GB-screen shell -- the Gen-2 sibling of
    run_u2c_trainer() above. Boot picker DOWN x(PICK_INDEX[which]+1) -> `which` row ->
    A -> S1 info -> A -> box grid (cold rom_gbsprite scan) -> START -> nav menu ->
    DOWN x3 -> Trainer -> A -> pdna_gbtrainer_gen2_card() (gbscr_open()'s OWN separate
    cold rom_gbui scan, same GB_ART_COLD_SETTLE ride-out run_u2c_trainer() needs)."""
    idx = PICK_INDEX[which]
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"u3_{which}_")
    print(f"== U3: {which}'s own trainer card (boot picker -> standalone -> Trainer) ==")

    s.run(700)
    s.press_n("DOWN", idx + 1, settle=gb_shots.SETTLE)      # Emerald (row 0) -> `which` row
    s.tap("A", settle=60)                                    # pick -> S1 info
    s.tap("A", settle=60)                                    # -> box grid (rom_gbsprite cold fetch)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 3)                                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=GB_ART_COLD_SETTLE)                    # Trainer -> pdna_gbtrainer_gen2_card()
                                                                # (gbscr_open's OWN cold rom_gbui scan)
    s.shot("01_page1_1to1", f"U3: {which}'s own trainer card, page 1, 1:1 -- NAME/ID No/"
                             "MONEY, the 5x7 card pic (row-major on Gold, column-major on "
                             "Crystal via rom_gbui_tile's own colmajor reorder), the STATUS "
                             "strip, #DEX (dex_owned popcount), PLAY TIME (colon currently "
                             "off-phase), and the BADGES(r) page hint")

    s.tap("SEL", settle=60)                                   # shell-wide toggle -> stretched
    s.shot("02_page1_stretched", "U3: SELECT stretches the SAME page to 240x160 -- the "
                                  "shell's own scale toggle, not a card-specific key")
    s.tap("SEL", settle=60)                                   # back to 1:1
    s.shot("03_page1_1to1_again", "U3: SELECT again returns to 1:1")

    # Cursor cycle order page 1 (G2C_*): NAME(0) ID(1) MONEY(2) TIME(3).
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("04_cursor_id", "U3: DOWN moves the cursor from NAME to ID No.")
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_cursor_money", "U3: DOWN again -> MONEY")

    s.tap("UP", settle=gb_shots.SETTLE)                        # MONEY -> ID
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # A on ID -> identity WARNING first
    s.shot("06_editor_id_warning", "U3: A on ID No. shows the identity-change WARNING "
                                    "first (gbtr_id_edit_ok(), the SAME sub-editor gate "
                                    "Gen 1's card and the plain page already share) -- "
                                    "ID has no cursor slot on Gen 1's own card at all, "
                                    "new to a GB card here")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # confirm -> num_entry opens
    s.shot("06b_editor_id_popup", "U3: A (yes) on the warning -- num_entry()'s existing "
                                   "pop-up opens (GBTR_ID, the same sub-editor "
                                   "pdna_gbtrainer_plain's own ID row uses); this is "
                                   "osk_search()'s own search-style keyboard, not a "
                                   "numeric keypad -- B is DELETE here (osk_core's own "
                                   "legend), SELECT cancels, START confirms")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                   # SELECT cancels osk_core (NOT B -- B deletes)
    s.shot("07_back_on_card", "U3: SELECT cancels the ID editor (osk_core's own cancel "
                               "key), back on page 1, ID selected, unchanged")

    # RIGHT flips to page 2 -- unlike Gen 1's card, this key pages rather than
    # cycling badges (the real game's own binding, design sec 1.2).
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("08_page2_1to1", "U3: RIGHT flips to page 2 -- the BADGES strip and the 8 "
                             "gym-leader faces (10 tiles each, 4 across the top row then "
                             "3+3 below); cursor lands on badge 0")

    s.tap("SEL", settle=60)
    s.shot("09_page2_stretched", "U3: page 2 stretched to 240x160")
    s.tap("SEL", settle=60)

    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # instant toggle badge 0
    s.shot("10_badge0_toggled", "U3: A instantly toggles badge 0 (the accepted "
                                 "deviation: a STATIC 2x2 BADGES-block overlay over the "
                                 "face's centre when owned -- the real game draws this "
                                 "as an animated OAM sprite the BG-tilemap oracle used "
                                 "to build this card cannot see at all)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # toggle back (no residual edit)
    s.shot("11_badge0_back", "U3: A again toggles badge 0 back")

    s.tap("LEFT", settle=gb_shots.SETTLE)                      # back to page 1
    s.shot("12_page1_again", "U3: LEFT flips back to page 1")

    # A REAL edit (a Johto badge left toggled) so B's commit prompt actually fires --
    # same reasoning as U2c's own shots 11/12/13: stays in this SAME visit rather than
    # re-entering the nav menu, which does not reliably land back on Trainer.
    s.tap("RIGHT", settle=gb_shots.SETTLE)                     # page 1 -> page 2
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # toggle badge 0 -- a REAL change
    s.shot("13_real_edit", "U3: badge 0 toggled and LEFT changed (unlike shots 10/11's "
                            "toggle-and-back, this one stays changed)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # B -> the actual commit prompt
    s.shot("14_commit_prompt", "U3: B with a real pending edit -> 'Save trainer "
                                "changes?' (app_confirm), the SAME dialog Gen 1's own "
                                "card and Gen 3's own card_editor use")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # decline -- discard the real edit too
    # The trainer screen (a nav-menu item, reached over the box grid) leaves an extra
    # UI layer or two behind it on the way back down -- same reasoning as
    # run_standalone()'s own "3x B to fully unwind" comment for its post-CREATE grid.
    # One extra B here is enough by hand-calibration; verified by shot 15 itself
    # showing the bare box grid, not a mon submenu.
    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("15_declined", "U3: declining a REAL edit also discards it -- the save was "
                           "never written (gbt_write never ran), back at the box grid")

    # Re-enter the card fresh for the START/plain-page shots (B on the front page above
    # exits the card entirely, same contract as U2c's own B).
    s.tap("START", settle=gb_shots.BIG_SETTLE)                 # box grid -> nav menu
    s.press_n("DOWN", 3)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                      # Trainer -> the card again
    s.tap("START", settle=gb_shots.BIG_SETTLE)                 # -> the plain field-complete page
    s.shot("16_plain_page", "U3: START reaches the full plain row-list page (coins, "
                             "mom's money/save, rival, mother, Kanto badges -- everything "
                             "not on the card face itself, unchanged data model)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # B on the plain page: discard, no commit
    s.shot("17_after_plain_back", "U3: B on the plain page discards (P1b's own rule) -- "
                                   "back at the box grid, not back on the card (the plain "
                                   "page is its own top-level fallback screen)")
    return s


# ---------------------------------------------------------------------------------
# BACKLOG #68b: cold-start timing, WITH vs WITHOUT the fused LOC payloads.
# ---------------------------------------------------------------------------------
# The rest of this file drives NAVIGATION (a fixed settle after each tap, calibrated
# by hand). This section instead MEASURES the thing those settles have to be
# generous enough to survive: the actual frame cost of the box grid's first,
# possibly-cold, art fetch. mGBA's Python binding has no "render finished" signal,
# so "stopped changing for a while" is the proxy -- same posture GB_ART_COLD_SETTLE's
# own 32,000-frame constant was hand-calibrated against.
GBA_FPS = 59.7275006103515625   # GBA's real refresh rate: 2^24 Hz / 280,896 cycles/frame
_MAX_FRAMES = 40000    # generous ceiling over GB_ART_COLD_SETTLE's own 32,000
_SAMPLE_EVERY = 5      # frames between crop samples while polling for "real paint"
_STABLE_WINDOW = 15    # a candidate crop must persist this many frames (3 samples) to count

# The left-panel "currently selected mon" portrait, in screen pixels (240x160) -- the
# ONE region gb_art_fetch()'s cold scan (BACKLOG #62/#68b) actually gates on Red.sav:
# the box grid's own small cell icons come from a separate, always-cheap source and
# are not gated by this backlog item -- but they paint on their OWN schedule, not
# immediately: measured empirically (BACKLOG #68b review D2), the icon cell is blank
# through frame 10, starts painting by frame 15, and is stable by frame 30, on both
# the LOC-seeded and --no-loc builds alike. With the LOC seed's ~10-20-frame portrait
# time, the portrait can appear BEFORE the box-grid icon finishes painting -- the two
# are independent races, not "icon first, portrait second". Measured empirically
# (BACKLOG #68b evidence run, 2026-09): on the SLOWER (--no-loc) build the box grid's
# shell paints almost immediately but this rectangle shows a "loading" checkerboard
# placeholder (colours (231,231,247)/(189,189,206)) for >14,000 frames before the
# real 4-shade Game Boy portrait replaces it; on the faster (default, LOC-seeded)
# build this same rectangle already shows the real portrait within ~20 frames, with
# no checkerboard phase ever observed at this script's sampling granularity.
_PORTRAIT_CROP = (5, 15, 70, 80)


def _crop_bytes(screen, box: tuple[int, int, int, int] = _PORTRAIT_CROP) -> bytes:
    return screen.to_pil().convert("RGB").crop(box).tobytes()


def _derive_checkerboard_ref(core_mod, image_mod, rom: Path) -> bytes:
    """Boots the SLOWER (--no-loc) image and captures _PORTRAIT_CROP's own "loading"
    placeholder, 300 frames after the box grid is requested -- confirmed by a manual
    probe (BACKLOG #68b) to still read as the placeholder that far in, and to stay
    that way for >14,000 further frames on Guy's own Red.gb corpus, i.e. nowhere near
    the real cold-scan actually finishing. Used as one of the two "not real art yet"
    references _measure_box_grid_cold_start() rules out before declaring victory."""
    s = gb_shots.Session(core_mod, image_mod, rom, Path("/tmp"), "measure_ref_")
    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=60)
    s.tap("A", settle=300)
    return _crop_bytes(s.screen)


def _measure_box_grid_cold_start(core_mod, image_mod, rom: Path,
                                 checkerboard_ref: bytes) -> tuple[int, bytes]:
    """Boots `rom`, drives the boot picker down to Red.sav (row 1), picks it, opens the
    S1 info page, then starts a frame-accurate timer at the exact frame the box grid
    is REQUESTED (right after the key-up edge of the second A press) and polls
    _PORTRAIT_CROP every _SAMPLE_EVERY frames until it stops matching EITHER of two
    "not real art yet" references: the S1 info page's own crop (still mid-transition)
    and `checkerboard_ref` (the "loading" placeholder) -- whichever a given build
    actually shows on its way to the real portrait, this is the first frame that is
    neither. Returns (frames_to_first_paint, final_screen_rgb_bytes) -- the second
    value is for a caller to diff the LOC-seeded and scanned paths' full final frames
    against each other (they must render the identical picture, portrait included)."""
    s = gb_shots.Session(core_mod, image_mod, rom, Path("/tmp"), "measure_")
    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)   # boot picker: Emerald row (0) -> Red row (1)
    s.tap("A", settle=60)                   # pick Red -> S1 info page
    info_page_crop = _crop_bytes(s.screen)

    # Same key-down/HOLD/key-up edge tap() uses, but WITHOUT its trailing settle --
    # the timer below starts counting from the exact frame the key-up edge lands,
    # which is the earliest frame the box-grid request could possibly begin.
    s.core.set_keys(raw=gb_shots.KEY["A"])
    s.run(gb_shots.HOLD)
    s.core.set_keys(raw=0)

    # A frame or two of blank/transitional content between "info page" and "real
    # portrait" is normal (the redraw is not atomic) -- a candidate crop value must
    # persist for _STABLE_WINDOW frames once it stops being either "not painted yet"
    # reference before it counts as the real paint, so a one-sample flicker (observed
    # in practice, BACKLOG #68b evidence run) is not mistaken for it.
    frame = 0
    candidate = None
    candidate_since = 0
    while frame < _MAX_FRAMES:
        s.core.run_frame()
        frame += 1
        if frame % _SAMPLE_EVERY:
            continue
        cur = _crop_bytes(s.screen)
        if cur == info_page_crop or cur == checkerboard_ref:
            candidate = None
            continue
        if cur == candidate:
            if frame - candidate_since >= _STABLE_WINDOW:
                return candidate_since, s.screen.to_pil().convert("RGB").tobytes()
        else:
            candidate = cur
            candidate_since = frame
    raise RuntimeError(f"{rom}: box grid portrait never left its pre-paint state "
                       f"within {_MAX_FRAMES} frames")


def run_cold_start_compare(core_mod, image_mod, loc_image: Path, noloc_image: Path,
                           out_dir: Path) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """BACKLOG #68b: measures and reports the cold-start frame cost of opening Red.sav's
    box grid on `loc_image` (fused WITH the rom_gb*_open_loc() records,
    tools/fuse_gb.py's default) versus `noloc_image` (the same build fused with
    --no-loc, i.e. today's behaviour before this backlog item) -- and proves the two
    images paint the IDENTICAL picture (a byte-diff of the final frame, expected 0)."""
    print("== #68b: cold-start timing, WITH vs WITHOUT the fused LOC payloads ==")
    checkerboard_ref = _derive_checkerboard_ref(core_mod, image_mod, noloc_image)
    loc_frames, loc_px = _measure_box_grid_cold_start(core_mod, image_mod, loc_image, checkerboard_ref)
    print(f"  WITH LOC   : {loc_frames} frames ({loc_frames / GBA_FPS:.2f} s emulated) "
          f"(+/- {_SAMPLE_EVERY} frames sampling granularity)")
    noloc_frames, noloc_px = _measure_box_grid_cold_start(core_mod, image_mod, noloc_image, checkerboard_ref)
    print(f"  WITHOUT LOC: {noloc_frames} frames ({noloc_frames / GBA_FPS:.2f} s emulated) "
          f"(+/- {_SAMPLE_EVERY} frames sampling granularity)")

    diff_bytes = sum(1 for a, b in zip(loc_px, noloc_px) if a != b)
    if len(loc_px) != len(noloc_px):
        diff_bytes += abs(len(loc_px) - len(noloc_px))
    print(f"  full-frame diff  : {diff_bytes} byte(s) differ between the two final frames")

    # The ART this backlog item actually gates on is _PORTRAIT_CROP (the currently
    # selected mon's big portrait, rendered by gb_art_fetch()) -- the rest of the box
    # grid (small cell icons, cursor) animates/updates on its own schedule, unrelated
    # to the cold-scan being measured here, so a whole-frame diff can show a handful
    # of incidental bytes even though the two paths are proven to load through the
    # SAME validated rom_gbsprite_open_loc() call either way. Isolate the claim this
    # backlog item actually makes: is the PORTRAIT itself pixel-identical.
    x0, y0, x1, y1 = _PORTRAIT_CROP
    from PIL import Image
    loc_crop = Image.frombytes("RGB", (240, 160), loc_px).crop(_PORTRAIT_CROP).tobytes()
    noloc_crop = Image.frombytes("RGB", (240, 160), noloc_px).crop(_PORTRAIT_CROP).tobytes()
    portrait_diff = sum(1 for a, b in zip(loc_crop, noloc_crop) if a != b)
    print(f"  portrait-only diff (the {x1 - x0}x{y1 - y0} region gb_art_fetch() actually "
          f"renders): {portrait_diff} byte(s) differ "
          f"({'IDENTICAL' if portrait_diff == 0 else 'MISMATCH'})")

    # Save both final frames as shots, captioned for gb_contact_sheet.py's "#68b:"
    # prefix match (FEATURE_TABLE's own new "delta-gb-loc" row, appended separately).
    ok: list[tuple[str, str]] = []
    from PIL import Image
    for label, px, frames in (("loc", loc_px, loc_frames), ("noloc", noloc_px, noloc_frames)):
        name = f"dgb_coldstart_{label}.png"
        path = out_dir / name
        Image.frombytes("RGB", (240, 160), px).save(path)
        caption = (f"#68b: Red.sav box grid, cold start {'WITH' if label == 'loc' else 'WITHOUT'} "
                   f"the fused rom_gb*_open_loc() record -- {frames} frames "
                   f"({frames / GBA_FPS:.2f} s emulated) to first stable paint")
        ok.append((name, caption))
        print(f"  [ok]   {name:32s} {caption}")
    return ok, []


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
    ap.add_argument("--image", type=Path,
                     help="pokedna-delta-gb.gba -- the ONE combined image (boot picker, "
                          "standalone GB mount, AND the nested NV_GB import all live here "
                          "since BACKLOG #68a retired the separate delta-gb-only image)")
    ap.add_argument("--shell-only", action="store_true",
                     help="U2a: only run_gbscreen_shell() against --image, skip the "
                          "boot-picker/standalone/NV_GB flows")
    ap.add_argument("--u2c-trainer", action="store_true",
                     help="U2c: only run_u2c_trainer() against --image (Red's own "
                          "trainer card), skip the other flows")
    ap.add_argument("--u3-trainer", choices=("gold", "crystal"),
                     help="U3: only run_u3_trainer() against --image for the named "
                          "game (Gold's or Crystal's own trainer card), skip the "
                          "other flows")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    ap.add_argument("--cold-start-compare", nargs=2, type=Path, metavar=("LOC_IMAGE", "NOLOC_IMAGE"),
                     help="BACKLOG #68b: measure+report the box-grid cold-start frame cost "
                          "on LOC_IMAGE (fused with the default rom_gb*_open_loc() records) "
                          "vs NOLOC_IMAGE (fused with `fuse_gb.py --no-loc`), diff their final "
                          "frames, and write both as shots into --out. Skips the normal "
                          "--image shot run entirely.")
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = gb_shots.load_mgba()

    if a.cold_start_compare:
        loc_image, noloc_image = a.cold_start_compare
        if not loc_image.is_file():
            sys.exit(f"--cold-start-compare: {loc_image}: not a file")
        if not noloc_image.is_file():
            sys.exit(f"--cold-start-compare: {noloc_image}: not a file")
        ok, skipped = run_cold_start_compare(core_mod, image_mod, loc_image, noloc_image, a.out)
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        return 0

    if not a.image:
        ap.error("--image is required unless --cold-start-compare is given")
    if not a.image.is_file():
        sys.exit(f"--image: {a.image}: not a file")

    ok, skipped = [], []
    if a.shell_only:
        try:
            sess = run_gbscreen_shell(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] gbscreen shell: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.u2c_trainer:
        try:
            sess = run_u2c_trainer(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u2c trainer: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.u3_trainer:
        try:
            sess = run_u3_trainer(core_mod, image_mod, a.image, a.out, a.u3_trainer)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u3 trainer ({a.u3_trainer}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

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
