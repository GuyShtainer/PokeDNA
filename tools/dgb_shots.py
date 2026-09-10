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
import functools
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402 -- Session, load_mgba, KEY, HOLD/SETTLE/BIG_SETTLE
import fuse_gb   # noqa: E402 -- BACKLOG #98 D3: reads the fused image's OWN directory
                 # (locate_record_permissive/read_record/parse_directory, the same
                 # code path `fuse_gb.py --check` uses) instead of assuming a fixed
                 # fuse order.


@functools.lru_cache(maxsize=None)
def gb_save_pick_index(image: Path) -> dict[str, int]:
    """BACKLOG #98 D3 (review-sonnet ab81b56): the boot picker's GB rows mirror
    fused_gb_save()'s own enumeration order -- SAV-type directory entries, in
    directory order, offset by +1 (row 0 is the Gen-3 save) -- so the picker row
    for a given save NAME depends on the fuse ORDER of THIS image, not some fixed
    Red/Gold/Crystal assumption. The old PICK_INDEX = {"red": 0, "gold": 1,
    "crystal": 2} hardcoded the Makefile's delta-gb recipe order and silently
    drove the wrong row against any image fused in a different order (e.g. a
    Crystal-first multi-ROM harness image built to reproduce BACKLOG #98 D1).

    Reads `image`'s own fused directory the same way `fuse_gb.py --check` does
    (locate_record_permissive -> read_record -> parse_directory) and maps each
    SAV entry's filename stem, lowercased, to its enumeration index -- e.g.
    {"gold": 0, "crystal": 1} for a Crystal-first two-save image, independent of
    what generation/order the caller expects. Memoized per image path: this
    script calls it once per shot, and the image never changes mid-run.
    """
    blob = image.read_bytes()
    rec_off = fuse_gb.locate_record_permissive(blob, str(image))
    off, size = fuse_gb.read_record(blob, rec_off)
    if not size:
        return {}
    entries = fuse_gb.parse_directory(blob, off, size)
    out: dict[str, int] = {}
    idx = 0
    for e in entries:
        if e["type"] != fuse_gb.TYPE_SAV:
            continue
        stem = Path(e["name"]).stem.lower()
        out[stem] = idx
        idx += 1
    return out

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
    idx = gb_save_pick_index(rom)[which]
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
    run_u2c_trainer() above. Boot picker DOWN x(gb_save_pick_index(rom)[which]+1) ->
    `which` row -> A -> S1 info -> A -> box grid (cold rom_gbsprite scan) -> START ->
    nav menu -> DOWN x3 -> Trainer -> A -> pdna_gbtrainer_gen2_card() (gbscr_open()'s
    OWN separate cold rom_gbui scan, same GB_ART_COLD_SETTLE ride-out
    run_u2c_trainer() needs)."""
    idx = gb_save_pick_index(rom)[which]
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


def run_u4_bag(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """U4 (BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.3): Red/Yellow's OWN
    Item bag + PC store on the shared GB-screen shell. `rom` must be a ONE-Gen-1-ROM
    fused image (tools/fuse_gb.py fed only Red.gb+Red.sav, or only Yellow.gb+
    Yellow.sav -- BACKLOG #98's known harness gap: the fused image lookup is keyed by
    GENERATION only, so a Red+Yellow-both image would serve whichever ROM the cache
    happens to answer with, not deterministically the one this run asked for). The
    boot picker therefore has exactly ONE GB row (index 1) regardless of `which` --
    same DOWN x1 -> A as run_u2c_trainer()'s own Red-only path, just generalised to
    a caption-only `which` label (no PICK_INDEX lookup needed with a single-ROM image).

    Nav: boot picker DOWN -> the GB row -> A -> S1 info -> A -> box grid (rom_gbsprite
    cold scan) -> START -> nav menu -> DOWN x7 (Party->Bank->Daycare->Trainer->Clock
    fix->Mirage->Pokedex->Bag, PDNA_NAV_ITEMS index 7) -> A -> pdna_gbbag() -- Gen 1,
    so this lands on pdna_gbbag_gen1_screen() (gbscr_open()'s own cold rom_gbui scan,
    separate cache from rom_gbsprite's box-grid one)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"u4_{which}_")
    print(f"== U4: {which}'s own Item bag (boot picker -> standalone -> Bag) ==")

    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # Emerald (row 0) -> the GB row (row 1)
    s.tap("A", settle=60)                                   # pick it -> S1 info
    s.tap("A", settle=60)                                   # -> box grid (rom_gbsprite cold fetch)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 7)                                     # Party -> ... -> Bag (index 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
                                                              # (gbscr_open's OWN cold rom_gbui scan)
    s.shot("01_items_row1", f"U4: {which}'s OWN Item bag, 1:1 centred -- the TEXTBOX "
                             "block's frame tiles (25-31, not tile 0 -- U2a's own "
                             "finding, re-confirmed here), 4 item rows visible (NOT "
                             "3 -- the design doc's own 'wMaxMenuItem=2' guess was "
                             "wrong), cursor on row 1 (the first entry)")

    s.tap("SEL", settle=60)                                     # shell-wide toggle -> stretched
    s.shot("01b_stretched", "U4: SELECT stretches the same list to 240x160 -- the "
                             "shell's own scale toggle, not a bag-specific key")
    s.tap("SEL", settle=60)                                     # back to 1:1
    s.shot("01c_1to1_again", "U4: SELECT again returns to 1:1")

    if which == "red":
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("02_items_row2", "U4: DOWN moves the cursor to row 2")
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("03_items_row3", "U4: DOWN again -> row 3")
        s.tap("DOWN", settle=gb_shots.SETTLE)                # row 3 -> row 4 (still visible)
        s.tap("DOWN", settle=gb_shots.SETTLE)                # row 4 -> scrolls: the window
                                                                # shifts down one row per press
        s.shot("04_scrolled", "U4: DOWN past the 4th visible row scrolls the window; "
                               "the cursor stays PINNED at the 3rd visible row (D3, "
                               "U4 review) and a down-scroll marker (font tile 0xEE) "
                               "BLINKS at (18,11) while more of the list is below -- it "
                               "is a real, visible tile (an earlier draft of this "
                               "caption claimed no marker exists at all; it does, this "
                               "shot's own blink phase just happened to land OFF)")

        s.tap("LEFT", settle=gb_shots.BIG_SETTLE)             # Items -> the PC store (same box)
        s.shot("05_pc_store", "U4: LEFT/RIGHT switches Items <-> the PC item store -- "
                               "the IDENTICAL box/list routine over a different pocket "
                               "(design doc's own citation: 'ITEMLISTMENU again, over "
                               "wNumBoxItems'); NOT independently pixel-dumped against "
                               "the real cart this slice (reaching a Pokemon Center PC "
                               "needs overworld navigation, out of this slice's time box)")
        s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)            # back to Items
        s.shot("05b_back_to_items", "U4: RIGHT switches back to Items")

        s.tap("A", settle=gb_shots.BIG_SETTLE)                # A on the selected item -> qty editor
        s.shot("06_qty_editor", "U4: A on the selected item opens the existing "
                                 "numeric editor (num_entry, 1-99) -- the SAME pop-up "
                                 "every other GB screen's own field edits use, not a "
                                 "game-art dialogue")
        s.tap("SEL", settle=gb_shots.BIG_SETTLE)              # osk_search: SELECT cancels
                                                                # (B is backspace, NOT cancel --
                                                                # p1c_shots.py's own precedent)

        s.tap("START", settle=gb_shots.BIG_SETTLE)            # -> the ITEM MENU (ADD/REMOVE/SWAP)
        s.shot("07_item_menu", "U4: START opens the item menu -- ADD ITEM / REMOVE / "
                                "SWAP / CANCEL; SWAP does NOT swap the row it was "
                                "opened on with the next one -- it ARMS a mark on "
                                "that row (the list's own 0xED cursor glyph, D10) and "
                                "returns to the list, where the game itself works: "
                                "move the cursor to a second row and A swaps the two, "
                                "or B drops the mark with nothing moved (see the "
                                "armed-SWAP shots below)")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # ADD ITEM -> id entry (osk_search)
        s.shot("08_add_item_id", "U4: ADD ITEM asks for a raw item id (1-250) -- no "
                                  "item-name table this slice (time-boxed, see "
                                  "pdna_gbbag.h); the list itself already prints "
                                  "'ITEM-n' for the same reason ('-' not '#': "
                                  "'#' has no Gen-1 glyph, D9). Cancelling either "
                                  "prompt now aborts the whole ADD (num_entry_opt, D5) "
                                  "instead of silently inserting id 1 x1.")

        # osk_search's own key contract (source/osk.c): A INSERTS the on-screen
        # keyboard's currently-highlighted glyph (row 0 is the digit row "1234567890",
        # cursor starts at (0,0) == '1'), B is BACKSPACE, START confirms, SELECT
        # cancels -- NOT "A confirms" (an earlier version of this script got that
        # wrong and silently mistyped every field; caught by looking at shot 07,
        # which showed the bag list with a corrupted quantity instead of the item
        # menu). N4 (review): the ORIGINAL version of this demo used id 77 (GOOD
        # ROD), already at qty 99 in Red.sav -- but id 77 is a Gen-1 KEY item
        # (gbb_is_g1_key_item(), 4a1afc8), so its row prints NO quantity at all;
        # "09b shows the row reading x99" was never true of that capture. id 20
        # (POTION) is an ORDINARY item, not in Red.sav's pocket yet -- ADD ITEM it
        # straight in at qty 99 below (typed directly, the brief's "qty editor, or
        # an edge save" alternative is not needed since ADD ITEM's own num_entry
        # IS a qty editor), so the saturating merge right after lands on a row
        # that actually prints a quantity.
        s.tap("B", settle=gb_shots.SETTLE)                     # clear the seeded "1"
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)          # keyboard cursor: col0 '1' -> col1 '2'
        s.tap("A", settle=gb_shots.SETTLE)                     # type '2' -> field "2"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)          # col1 '2' -> col9 '0' (no B: '2' must stay typed)
        s.tap("A", settle=gb_shots.SETTLE)                     # type '0' -> field "20"
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm id=20 (POTION) -> quantity entry
        s.shot("08b_add_item_qty", "U4: then a quantity (1-99), the same num_entry -- "
                                    "id 20 (POTION, an ORDINARY item) typed via the "
                                    "digit row")
        s.tap("B", settle=gb_shots.SETTLE)                     # clear the seeded "1"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)          # '1' -> '9' (row 0, col 8)
        s.tap("A", settle=gb_shots.SETTLE)                     # type '9' -> field "9"
        s.tap("A", settle=gb_shots.SETTLE)                     # type '9' again (cursor unmoved) -> field "99"
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm qty=99 -> gbb_insert(...,20,99): new entry
        s.shot("08c_potion_planted", "U4: id 20 (POTION) inserted fresh at qty 99 -- "
                                      "gbb_insert() takes the FREE-SLOT path (no "
                                      "existing id-20 entry to merge into), landing "
                                      "on the list's own last row; the qty editor "
                                      "step the N4 brief also allows (A on a row) is "
                                      "therefore not separately needed here -- ADD "
                                      "ITEM was typed straight to the cap")

        s.tap("START", settle=gb_shots.BIG_SETTLE)             # -> the item menu again, cursor still on POTION
        s.tap("A", settle=gb_shots.BIG_SETTLE)                 # ADD ITEM -> id entry again
        s.tap("B", settle=gb_shots.SETTLE)                     # clear seeded "1"
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)          # col0 -> col1 '2'
        s.tap("A", settle=gb_shots.SETTLE)                     # type '2' -> field "2"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)          # col1 -> col9 '0' (no B: keep the '2')
        s.tap("A", settle=gb_shots.SETTLE)                     # type '0' -> field "20" again
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm id=20 -> quantity entry
        s.tap("B", settle=gb_shots.SETTLE)                     # clear seeded "1"
        s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)          # '1' -> '5' (row 0, col 4)
        s.tap("A", settle=gb_shots.SETTLE)                     # type '5'
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm qty=5 -> gbb_insert(...,20,5): MERGE path
        s.shot("09_saturation_refusal", "U4: merging qty 5 into id 20/POTION (already "
                                         "at the 99 cap from the ADD above) saturates "
                                         "and refuses -- gbb_insert() SETS the "
                                         "existing stack to the cap (99) and returns "
                                         "GBB_ERR_QTY (gb_bag.c's own 'sum > cap' "
                                         "branch WRITES list->entries[i].qty = "
                                         "GBB_QTY_CAP, it does not merely refuse); "
                                         "gbbag_start_menu's own msg_wait('SATURATED', "
                                         "...) reports it. The WRITE still happens "
                                         "here (99 -> 99) -- it is a no-op only "
                                         "because POTION was already at the cap; nothing "
                                         "about the mechanism itself skips the write.")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                 # dismiss the msg_wait -- back on the list
        # ADD ITEM's own `*sel = l->count - 1` leaves the cursor on the LAST entry --
        # POTION is that last entry (it was appended fresh above and nothing since
        # has added/removed a row), so it is already in frame with no scrolling.
        s.shot("09b_after_add", "U4: after dismissing the refusal, the cursor is "
                                 "already on POTION's own row (ADD ITEM's own "
                                 "'*sel = last entry' rule, unchanged since POTION "
                                 "was appended) -- it reads x99, confirming the "
                                 "saturating write landed exactly where it started "
                                 "(99 -> 99, see the 09 caption above); an ORDINARY "
                                 "item's row, unlike id 77/GOOD ROD (a Gen-1 KEY "
                                 "item, prints no quantity at all).")

        # The saturation refusal above made NO byte change (99 -> clamped-to-99 is a
        # true no-op), so B here would take the silent memcmp-no-op path, not the
        # commit prompt -- a REAL edit is needed first. REMOVE the currently selected
        # entry (POTION, left selected by the ADD ITEM path above) via the item menu.
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # -> the item menu again
        s.tap("DOWN", settle=gb_shots.SETTLE)                  # ADD ITEM -> REMOVE
        s.tap("A", settle=gb_shots.BIG_SETTLE)                 # REMOVE the selected entry -- a REAL change
        s.shot("09c_removed", "U4: REMOVE deletes POTION -- a real, "
                               "persisted-if-confirmed change (unlike the saturation "
                               "attempt above)")

        s.tap("B", settle=gb_shots.BIG_SETTLE)                # B -> the commit prompt (a real edit pending)
        s.shot("10_commit_prompt", "U4: B with a real pending edit -> 'Save bag "
                                    "changes?' (app_confirm), the same dialog every "
                                    "other GB screen's own commit uses")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                # decline -- discard the edit
        s.shot("11_declined", "U4: declining discards the edit -- gbb_write never ran, "
                               "back at the box grid")

        # N6(a): the armed-SWAP state (D10) -- START > SWAP on a row ARMS a mark,
        # it does not swap on the spot. Re-enter the bag screen (declining above
        # never persisted anything, so gbb_read() below re-reads the ORIGINAL
        # unedited pocket -- row 0/row 1 are back to their pristine ids).
        # D-reentry (this pass, empirically): BIG_SETTLE alone is NOT enough idle
        # time for the box grid to accept a fresh START right after RETURNING
        # from the bag screen -- measured with a throwaway diagnostic script: the
        # very next START press was silently swallowed (no menu opened, same
        # frame) with only BIG_SETTLE=40 frames of idle first, but succeeded with
        # 250. The FIRST entry into the bag screen (this function's own opening
        # sequence, above) never hits this because it follows a `run(GB_ART_COLD_
        # SETTLE)` (32,000 frames) already, not a bare BIG_SETTLE return.
        s.run(250)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)            # Party -> ... -> Bag (index 7)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
        s.shot("12_reentry_pristine", "U4: N6(a) setup -- re-entering the bag screen "
                                       "after declining shows the ORIGINAL, unedited "
                                       "Items list (the 09c/10/11 REMOVE was never "
                                       "written)")
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu, cursor on row 0
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP (csel 2)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # SWAP: arms row 0 as swap_src, returns
        s.tap("DOWN", settle=gb_shots.SETTLE)                   # move the cursor OFF the source row --
                                                                 # only now does row 0's own 0xED marker
                                                                 # stop being hidden behind is_sel
        s.shot("12_swap_armed", "U4: SWAP arms row 0 (the mark glyph -- the list's "
                                 "own 0xED cursor tile, D10 -- stays lit on row 0) "
                                 "and returns to the list; the cursor itself is now "
                                 "on row 1 (its OWN 0xED), so TWO rows show the same "
                                 "glyph at once -- this is the armed state, not a "
                                 "swap that already happened")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the destination -> the actual swap
        s.shot("12b_swap_done", "U4: A on the destination (row 1) performs the swap "
                                 "-- row 0 and row 1's entries have traded places; "
                                 "the mark is gone (g1_swap_active cleared) and "
                                 "neither row shows a stray 0xED anymore")

        # Undo this real edit so the B-cancel proof below starts from a clean diff
        # baseline: swap row 0/row 1 back, then leave. The cursor is CURRENTLY on
        # row 1 (12b_swap_done's own A-on-destination left `sel` there, unchanged
        # by the swap itself) -- SWAP arms whatever row the cursor is ON, so
        # arming here grabs row 1 (id 206, post-swap) as the source; move UP to
        # row 0 (id 205) as the destination, not DOWN, to swap the SAME pair back.
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # re-arm SWAP on row 1 (cursor's current row)
        s.tap("UP", settle=gb_shots.SETTLE)                     # cursor -> row 0 (the OTHER half of the pair)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # swap back -> pristine order restored
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # -> the no-op path: two swaps of the SAME
                                                                 # pair restore identical bytes, memcmp(bag,t0)
                                                                 # == 0 again, straight back to the box grid
        s.shot("12c_swap_undone", "U4: swapping row 0/row 1 back and leaving -- two "
                                   "swaps of the same pair restore the identical "
                                   "bytes (memcmp(bag,t0)==0), so this is the SAME "
                                   "silent no-op path as an unedited visit, straight "
                                   "back to the box grid, no confirm dialog")

        # N6(a) part 2: B drops an armed mark WITHOUT moving anything -- the KEY_B
        # handler's own g1_swap_active branch never touches bag->pockets at all
        # (source/pdna_gbbag.c pdna_gbbag_gen1_screen, the `if (g1_swap_active) {
        # g1_swap_active = false; ...; continue; }` arm under KEY_B) -- a true
        # 0-byte change, provable from the code path itself: that branch contains
        # no assignment to any bag field, only the repaint. Demonstrated here by
        # what the emulator CAN show: leaving the screen right after affords no
        # confirm dialog at all, the same silent path 12c above takes for a real
        # no-op -- if B-drop-mark had mutated anything, `want_commit && memcmp(...)
        # != 0` would have popped 'Save bag changes?' instead.
        # 12c_swap_undone above already LEFT the bag screen (its own B fell
        # through to the box grid, the same "no-op path" 11_declined took) -- a
        # full re-entry is needed here, not just an item-menu re-open.
        s.run(250)                                              # re-entry idle (see the D-reentry note above)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)            # Party -> ... -> Bag (index 7)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu, cursor back on row 0
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # arm SWAP on row 0
        s.tap("DOWN", settle=gb_shots.SETTLE)                   # cursor -> row 1 (source's mark visible)
        s.shot("13_swap_armed_again", "U4: N6(a) part 2 setup -- SWAP armed on row 0 "
                                       "again, cursor moved to row 1")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B: drop the mark, nothing moves
        s.shot("13b_mark_dropped", "U4: B drops the armed mark -- row 0's own 0xED "
                                    "is gone, row 1 still shows the cursor's own "
                                    "0xED, and NEITHER row's underlying entry "
                                    "changed (compare against 12_swap_armed above: "
                                    "same ids, same quantities, same order)")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B again (nothing armed) -> leave
        s.shot("13c_left_no_prompt", "U4: leaving right after -- straight back to "
                                      "the box grid, no 'Save bag changes?' prompt "
                                      "at all. That prompt only ever fires when "
                                      "memcmp(bag,t0)!=0 (pdna_gbbag() below "
                                      "pdna_gbbag_gen1_screen); its absence here IS "
                                      "the byte-compare proof for the B-cancel-mark "
                                      "path: 0 bytes changed, not merely 'looks "
                                      "unchanged on screen'.")

        # N6(b): BAD ID refusals -- ADD ITEM with id 0 and id 251 (Gen 1's own
        # range is 0x01..0xFA == 1..250, gb_bag.h's own VALID ITEM IDS comment;
        # 0 and 0xFB==251 are both one step outside either edge).
        s.run(250)                                              # re-entry idle (see the D-reentry note above)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # back into the bag screen
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu (csel 0 == ADD ITEM)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry, seeded "1"
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> field empty
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id="" -- num_entry_opt treats an
                                                                 # empty field as 0, the refused id below --
                                                                 # -> QUANTITY prompt next (seeded "1", already
                                                                 # valid; gbb_insert() only runs after BOTH
                                                                 # prompts confirm, so id alone shows nothing yet)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=1 (seeded, unedited) -> NOW
                                                                 # gbb_insert(id=0, qty=1) actually runs
        s.shot("14_bad_id_zero", "U4: N6(b) -- ADD ITEM with id 0 (the field left "
                                  "empty, which num_entry_opt reads back as 0) -> "
                                  "gbb_insert() returns GBB_ERR_BADID -- "
                                  "msg_wait('BAD ID', ..., 'That item id does not "
                                  "exist.')")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss

        # N6(b) part 2 -- id 251: one past Gen 1's last legal id (250). R1 (the
        # re-verify-3 one-liner): the ID prompt's OSK cap is 999 and the value is
        # clamped to 0xFF, so 251 reaches gbb_insert() as 251 and lands on its own
        # GBB_ERR_BADID branch -> the same 'BAD ID' dialog as id 0. (Before R1 the
        # prompt clamped to 250 = TM50 and INSERTED it silently.)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # item menu again
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry, seeded "1", cursor col0
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> field empty, cursor col0
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)           # col0 -> col1 '2'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '2' -> field "2"
        s.press_n("RIGHT", 3, settle=gb_shots.SETTLE)           # col1 -> col4 '5'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '5' -> field "25"
        s.press_n("RIGHT", 6, settle=gb_shots.SETTLE)           # col4 -> col10 mod 10 == col0 '1' (osk.c's
                                                                 # own KEY_RIGHT wraps `(cc + 1) % rowlen`,
                                                                 # source/osk.c line 223 -- the digit row is
                                                                 # 10-wide, so RIGHT wraps circularly)
        s.tap("A", settle=gb_shots.SETTLE)                      # type '1' -> field "251"
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id "251" -> QUANTITY prompt (seeded "1")
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=1 -> gbb_insert(id=251) -> GBB_ERR_BADID
        s.shot("15_id251_bad_id", "U4: N6(b) -- typing id 251 (one past Gen 1's last "
                                   "legal id) now reaches gbb_insert() unchanged and is "
                                   "refused with the same 'BAD ID / That item id does not "
                                   "exist.' dialog as id 0 -- this frame is pixel-identical "
                                   "to shot 14 BY DESIGN (the dialog never echoes the typed "
                                   "id; allow_same) -- R1: the ID prompt no longer clamps "
                                   "to 250; nothing was inserted.", allow_same=True)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss; nothing to undo

        # N6(c): BAD QUANTITY refusals -- qty 0 and qty 100 (valid range 1..99).
        # A valid id is needed to reach the quantity prompt at all; id 20 (POTION,
        # not currently in the pocket, same id N4 used) keeps this an INSERT, not
        # a merge, so the refusal is unambiguously about the typed quantity.
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry
        s.tap("B", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)           # -> '2'
        s.tap("A", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)           # -> '0'
        s.tap("A", settle=gb_shots.SETTLE)                      # id "20"
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> quantity entry
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> empty (reads back as 0)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=0
        s.shot("16_bad_qty_zero", "U4: N6(c) -- qty 0 (the field left empty) -> "
                                   "gbb_insert() returns GBB_ERR_QTY, and because "
                                   "the TYPED value (0) was itself outside 1..99 "
                                   "(qty_in_range false), gbbag_start_menu's own "
                                   "branch reports 'BAD QUANTITY' / 'Quantity must "
                                   "be 1-99.' -- NOT 'SATURATED' (that wording is "
                                   "reserved for a legal typed value that overflowed "
                                   "an existing stack on merge, see the N4 shots)")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss
        s.run(80)                                               # extra margin -- this stretch flaked during
                                                                 # authoring under BIG_SETTLE alone (mGBA
                                                                 # timing, the same class the 04/blink-marker
                                                                 # caption already documents)

        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry
        s.tap("B", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # id "20" again
        s.run(80)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> quantity entry, seeded "1", cursor col0
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> field empty, cursor col0
        s.tap("A", settle=gb_shots.SETTLE)                      # col0 IS '1' -- type it directly -> "1"
        s.press_n("RIGHT", 9, settle=gb_shots.SETTLE)           # col0 -> col9 '0'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '0' -> "10"
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # cursor unmoved (still col9 '0') -> "100"
        s.shot("17_bad_qty_100_typed", "U4: N6(c) -- '100' typed into the SAME "
                                        "quantity prompt as 16 (one past the 99 "
                                        "cap, not clamped by the OSK -- num_entry_"
                                        "opt's own maxv for THIS prompt is 999, "
                                        "not 99, exactly so a typed 100 reaches "
                                        "gbb_insert()'s own validation instead of "
                                        "being silently clamped first, D5)")
        s.run(80)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=100 -> gbb_insert() returns
                                                                 # GBB_ERR_QTY, qty_in_range false again -- the
                                                                 # SAME 'BAD QUANTITY' / 'Quantity must be
                                                                 # 1-99.' dialog as 16 (msg_wait's own text
                                                                 # never echoes the typed value, so the two
                                                                 # dialogs are PIXEL-IDENTICAL -- not
                                                                 # re-captured here on purpose: gb_shots.py's
                                                                 # own Session.shot() refuses a pixel-identical
                                                                 # repeat as a likely driver bug, and here it
                                                                 # would be right to be suspicious of a NEW
                                                                 # bug except this one really is the same
                                                                 # dialog by design; 17_bad_qty_100_typed above
                                                                 # is the honest proof of what was actually
                                                                 # typed instead)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss (msg_wait's own "Press A")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # leave -- every refusal above made no edit,
                                                                 # so this is the silent no-op path again
        s.shot("18_left_after_refusals", "U4: leaving after every N6(b)/(c) refusal "
                                          "-- no confirm dialog, same no-op-path "
                                          "proof as 13c above: none of the BAD ID / "
                                          "BAD QUANTITY attempts wrote anything")

    return s


def run_u4_empty(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """N6(e): Red's own Item bag with an EMPTY Items pocket (count 0) -- `rom` must
    be fused with a Red save whose Items pocket was zeroed by docs/shots/rvu4/
    mkbag.c (no ids given on argv -- gbb_write() then persists count=0). Same nav
    as run_u4_bag()'s own red case: boot picker DOWN -> A -> A -> box grid -> START
    -> nav DOWN x7 -> A."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "u4_empty_")
    print("== N6(e): Red's own Item bag, Items pocket count == 0 ==")
    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=60)
    s.tap("A", settle=60)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)
    s.shot("01_empty_pocket", "N6(e): Items pocket count == 0 -- the list is CANCEL "
                               "alone (D3's own '+ the CANCEL row' rule over a "
                               "0-entry pocket: total = 0 + 1 = 1 row), cursor "
                               "pinned on it. Byte-for-byte oracle proof: the real "
                               "cartridge on the SAME save (docs/shots/rvu4/"
                               "oracle.py --tag red_empty) reads grid[4][5] == "
                               "0xed (the cursor glyph) and grid[4][6..11] == "
                               "0x82,0x80,0x8d,0x82,0x84,0x8b ('C','A','N','C','E',"
                               "'L') at the SAME screen position -- see this run's "
                               "own paste in the U4 report.")
    return s


def run_u5_pack(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """U5 (BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.4): Gold/Silver/
    Crystal's OWN Pack + PC store on the shared GB-screen shell -- the Gen-2
    sibling of run_u4_bag() above. `rom` must be a ONE-Gen-2-ROM fused image
    (tools/fuse_gb.py fed only Gold.gbc+Gold.sav, or only Crystal.gbc+
    Crystal.sav -- BACKLOG #98's known harness gap, same reasoning as U4's own
    single-ROM requirement). A single-ROM image skips the boot picker entirely
    (gb_delta_pick_save()'s own `if (n == 1) return 0`, same as run_d7_gold()'s
    own doc comment) -- one A tap reaches S1 info -> box grid directly.

    Nav: A (S1 info) -> box grid (rom_gbsprite cold scan) -> START -> nav menu
    -> DOWN x7 (Party->Bank->Daycare->Trainer->Clock fix->Mirage->Pokedex->Bag,
    PDNA_NAV_ITEMS index 7, SAME row order as Gen 1 -- nav_avail's GB_TABLE has
    one row per NV_* id with a per-generation COLUMN) -> A -> pdna_gbpack_gen2_
    screen() (gbscr_open()'s OWN cold rom_gbui scan)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"u5_{which}_")
    print(f"== U5: {which}'s own Pack (single-ROM image -> standalone -> Pack) ==")

    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 7)                                     # Party -> ... -> Bag (index 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbpack_gen2_screen()
    s.shot("01_items", f"U5: {which}'s OWN Pack, ITEMS pocket, 1:1 -- the "
                        "pocket picture (PackGFX/pack_m, real ROM art) and the "
                        "static header/nameplate art (the derived PACKMENU "
                        "block), 5 item rows visible, cursor on row 1")

    s.tap("SEL", settle=60)
    s.shot("01b_stretched", "U5: SELECT stretches the same list to 240x160")
    s.tap("SEL", settle=60)
    s.shot("01c_1to1_again", "U5: SELECT again returns to 1:1")

    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls
    s.shot("02_balls", "U5: RIGHT cycles to the BALLS pocket -- a different "
                        "nameplate label + pocket picture slice (PackGFX ROM "
                        "index 3), the CANCEL row (real-cartridge fact, corrects "
                        "an earlier design-doc guess that Gen 2 has no CANCEL "
                        "row -- confirmed live on this exact save)")
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Balls -> Key items
    s.shot("03_key_items", "U5: RIGHT again -> KEY ITEMS -- no quantity column "
                            "at all (blank, not '-'), matching the real "
                            "cartridge's own posture for this pocket")
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Key items -> TM/HM
    s.shot("04_tmhm", "U5: RIGHT again -> TM/HM -- the two-digit TM/HM number "
                       "prefix (cols 5-6, before the cursor) plus a 'TMnn'-style "
                       "fallback name (no move-name table this slice) and its "
                       "own count column. Also D4's own fix: every row here is "
                       "an OWNED TM/HM (this save's TM09 is missing and never "
                       "appears as a blank or zero-count row, matching the "
                       "real cartridge's TMHM_DisplayPocketItems)")

    # D3 fix (review-opus ac9ffc0): the cursor walks all ROWS_VISIBLE (5) rows
    # before `top` moves, matching the real cartridge's own gold_down4 ground
    # truth -- NOT Gen 1's bag-style pin-at-slot-2 the earlier draft borrowed.
    # This save's TM/HM pocket has 56 owned entries (D4), plenty to scroll.
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)
    s.shot("04b_scrolled_unpinned", "U5 D3 fix: TM/HM, 4 DOWNs from the top -- "
                                     "the cursor is on the FIFTH visible row "
                                     "and the list has NOT scrolled yet (`top` "
                                     "is still 0) -- the cursor walks the whole "
                                     "visible page before scrolling starts")
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)
    s.shot("04c_scrolled_further", "U5 D3: four MORE DOWNs (8 total) -- now "
                                    "the list has actually scrolled, `top` "
                                    "tracking the cursor one row at a time")
    s.press_n("UP", 8, settle=gb_shots.SETTLE)              # back to row 0 for the rest of the flow

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (TM/HM: PC STORE/CANCEL only,
                                                              # csel starts on PC STORE, no DOWN needed)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle -> PC store
    s.shot("05_pc_store", "U5: START > PC STORE toggles to the PC item store -- "
                           "reuses the Items pocket's own art column (not "
                           "independently pixel-dumped this slice, see "
                           "pdna_gbpack.h). D6 fix: the description box now "
                           "prints 'PC ITEM STORE' -- the earlier draft's "
                           "empty box left the store visually identical to "
                           "the Items pocket")

    # D7 fix (review-opus ac9ffc0): ADD ITEM from the PC store is no longer
    # refused -- it is its own undifferentiated list, not one of the four
    # real bag pockets the Items-only fallback rule was meant to guard. This
    # save's PC store is already at capacity (BAG FULL on a completed add is
    # a real, expected refusal -- a full pocket, not a wrong one) so the demo
    # only needs to show the id-ENTRY screen opening (proof ADD ITEM is no
    # longer refused outright), then cancel out with SELECT (osk_search's own
    # cancel, same as shot 08's qty editor) rather than complete the insert --
    # osk_search's `continue` lands back in the SAME open PACK MENU (csel
    # still 0), so no extra START tap is needed before the DOWN x3 below.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (PC store, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry (seeded "1")
    s.shot("05b_pc_store_add_item", "U5 D7 fix: START > ADD ITEM from the PC "
                                     "ITEM STORE opens the id entry instead of "
                                     "refusing 'WRONG POCKET' (the earlier "
                                     "draft's behaviour) -- this save's PC "
                                     "store happens to already be full, so "
                                     "this demo cancels out rather than "
                                     "complete an insert that would correctly "
                                     "show BAG FULL, a different, expected "
                                     "refusal")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # osk_search: SELECT cancels -> back in the menu

    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle back -> Pack (still on TM/HM's own cyc)
    s.shot("06_back_to_pack", "U5: START > PACK toggles back")

    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # TM/HM -> Key items
    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # Key items -> Balls
    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # Balls -> Items
    s.shot("07_left_wraps_to_items", "U5: LEFT cycles the other way -- back on "
                                      "ITEMS")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the selected item -> qty editor
    s.shot("08_qty_editor", "U5: A on the selected item opens the existing "
                             "numeric editor (num_entry, 1-99) -- the SAME "
                             "pop-up every other GB screen's own field edits "
                             "use")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # osk_search: SELECT cancels

    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (ADD ITEM/REMOVE/SWAP/PC STORE/CANCEL)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM on Balls -> WRONG POCKET refusal
    s.shot("09_wrong_pocket_refusal", "U5: START > ADD ITEM from the BALLS "
                                       "pocket refuses outright -- 'WRONG "
                                       "POCKET' / 'Add items from the Items "
                                       "pocket.' (per-item pocket membership "
                                       "was not located this slice; the "
                                       "brief's own sanctioned fallback is "
                                       "Items-only ADD ITEM, not a silent "
                                       "wrong-pocket accept)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back in the PACK MENU (msg_wait's
                                                              # own `continue` loops the menu, does NOT
                                                              # close it -- gbpack_start_menu's own ADD ITEM
                                                              # branch, csel unchanged at 0)
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP -> PC STORE -> CANCEL
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # CANCEL -> back to the list (still Balls)
    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # Balls -> Items

    # Saturation refusal on an ORDINARY item: ADD ITEM the currently-selected
    # pocket's own id 1 (not present yet) at qty 99 (a SILENT success -- no
    # message, gbb_insert() returns GBB_OK -- so the menu loop's own `continue`
    # lands right back on ADD ITEM with NO extra tap needed), then ADD ITEM id
    # 1 again at qty 5, which MERGES into the fresh 99 stack and overflows the
    # cap -- THIS one does show SATURATED. osk_search's own contract: row 0 is
    # the digit row "1234567890", cursor starts at (0,0) == '1', A inserts the
    # highlighted glyph, B backspaces, START confirms, SELECT cancels (U4's
    # own precedent, run_u4_bag() above).
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (Items, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry (seeded "1")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id=1 -> quantity entry
    s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1"
    s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)           # col0 -> col8 '9'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '9'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '9' again -> field "99"
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=99 -> gbb_insert(id=1,99): FREE-SLOT
                                                              # path, GBB_OK, NO message -- gbpack_start_menu's
                                                              # own ADD ITEM branch falls through to `return 0`
                                                              # unconditionally after a COMPLETED add (success
                                                              # OR an error message dismissed), closing the
                                                              # menu straight back to the LIST -- only the
                                                              # WRONG-POCKET early-refuse `continue`s and stays
                                                              # in the menu; this is NOT that case.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU again (fresh open, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM AGAIN -> id entry (seeded "1")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id=1 -> quantity entry
    s.tap("B", settle=gb_shots.SETTLE)
    s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)           # col0 -> col4 '5'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '5'
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=5 -> merge overflows 99 -> SATURATED
    s.shot("10_saturation_refusal", "U5: re-adding id 1 at qty 5 merges into "
                                     "the existing (already-99) stack -- "
                                     "gbb_insert() saturates it at the cap and "
                                     "reports SATURATED (same mechanism as "
                                     "U4's own N6(c))")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> gbpack_start_menu's own ADD ITEM
                                                              # branch falls through to `return 0` after this
                                                              # (a COMPLETED add, message or not) -- back at
                                                              # the LIST, not the menu.

    # SWAP: arm row 0, move down, confirm the destination. The cursor is
    # currently on the LAST real row (the fresh id-1 insert, ADD ITEM's own
    # `*sel = l->count - 1` rule) -- move it UP first so SWAP arms a row with
    # a REAL row below it, not the trailing CANCEL row (arming the last real
    # row and pressing DOWN would land on CANCEL, which the destination guard
    # correctly refuses -- sel < cnt is false for it -- but that is a
    # different, less illustrative demo than an actual two-item swap).
    s.press_n("UP", 2, settle=gb_shots.SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (fresh open, csel=0=ADD ITEM)
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # SWAP: arms row 0, returns to the list
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # cursor off the source row
    s.shot("11_swap_armed", "U5: SWAP arms row 0 (the mark stays lit there) "
                             "and returns to the list -- pick-source-then-"
                             "destination, same semantic as U4's own Gen-1 "
                             "SWAP")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the destination -> the actual swap
    s.shot("11b_swap_done", "U5: A on the destination performs the swap -- "
                             "both rows traded places, the mark is gone")
    # Arm again and drop with B instead of swapping.
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # re-arm on the current row
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B drops the mark, nothing moves
    s.shot("11c_swap_dropped", "U5: B drops an armed SWAP mark without leaving "
                                "the screen or moving anything -- the row's own "
                                "0xED marker is gone, no entries changed")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B with a real pending edit -> commit prompt
    s.shot("12_commit_prompt", "U5: B with a real pending edit -> 'Save pack "
                                "changes?' (app_confirm), the same dialog "
                                "every other GB screen's own commit uses")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline
    s.shot("13_declined", "U5: declining discards the edit -- gbb_write never "
                           "ran, back at the box grid")

    return s


def run_d7_gold(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """N6(f): Gold's own START > BAG refusal, from a COMMITTED driver (`rom` must
    be a Gold-only fused image, tools/fuse_gb.py fed Gold.gbc+Gold.sav -- the
    earlier docs/shots/u4/d7_gold_01_bag_refusal.png came from an ad-hoc,
    non-driver capture; this reproduces it from code). Gen 2's NV_BAG row is
    NAV_COMING_SOON (source/nav_avail.c: 'The Bag is coming soon.') -- Gen-1's own
    Bag screen (pdna_gbbag.c) is Gen-1 only, Gen 2's Pack is a later slice.
    Nav: gb_delta_pick_save()'s own `if (n == 1) return 0` (source/pdna_main.c
    ~8913) means a SINGLE-ROM fused image (this one -- only Gold, no Emerald/
    Gen-3 save fused, so gb_delta_boot_pick()'s own picker never triggers either,
    same gate) skips straight to the GB save's S1 info page with NO boot-picker
    taps at all -- unlike run_u4_bag()'s own doc comment above (which describes
    the combined multi-ROM image's picker), this image needs exactly ONE tap
    (A: S1 info -> box grid), not DOWN+A+A. Then: START -> nav DOWN x7
    (Party->Bank->Daycare->Trainer->Clock->Mirage->Pokedex->Bag, SAME NV_* row
    order as Gen 1 -- nav_avail's GB_TABLE has one row per NV_* id with a
    per-generation COLUMN, not a per-generation row order) -> A -> app_nav_refuse()."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "d7_gold_")
    print("== N6(f): Gold's own START > BAG refusal ==")
    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (no DOWN/second A needed)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 7)
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("01_bag_refusal", "N6(f): Gold, START > (nav DOWN x7) > Bag -> "
                              "app_nav_refuse() -- 'COMING SOON' / 'The Bag is "
                              "coming soon.' (nav_avail.c's own GB_TABLE[NV_BAG][1] "
                              "for SE_KIND_GEN2). Gen-1's own pdna_gbbag.c screen "
                              "is Gen-1 only (pdna_gbbag() itself refuses "
                              "s->gen != GB_GEN1); Gen 2's Pack is a later slice, "
                              "not this one.")
    return s


# ---------------------------------------------------------------------------------
# BACKLOG #104 R1: KEEP AS IS / MAKE LEGAL on a Gen 3 -> Game Boy paste.
# ---------------------------------------------------------------------------------

def run_r1_xfer(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #104 R1 (docs/TRANSFER-ROUNDTRIP-DESIGN.md section 3c/4): the KEEP AS
    IS / MAKE LEGAL choice gb_paste_hook now offers between the existing loss screen
    and the box-writable check, for a paste whose species is standing below its
    evolution's minimum level.

    `rom` must be `pokedna-delta-artless.gba` fused with THREE payloads, in this
    order (tools/fuse_sav.py then tools/fuse_gb.py, --check confirms all four
    directory entries land): an ordinary Gen-3 .sav (any works; its own contents are
    never read by this flow -- it only exists so the boot picker has a row 0 to skip
    past), an 80-byte raw Gen-3 box record via `fuse_sav.py --clip` (screenshot-only
    hook, docs already ship: "so PASTE (GB) on an empty cell reachable without a
    Gen-3 session ever having been open") built with a REAL species standing below
    its own evolution floor (Charizard, species 6, at level 20 -- pk_evo_min_level(6)
    == 36, walking Charmander -> L16 Charmeleon -> L36 Charizard), and ONE Game Boy
    ROM+save via `fuse_gb.py` ("one ROM per image" per the R1 brief).

    GOLD, NOT RED -- a deliberate substitution from the brief's own "Red-only"
    wording, found and documented rather than silently swapped: gb_paste_hook's
    Gen-1 branch needs a base-stats ROM (`GbGen1Base`, species types/catch rate --
    Gen-1 records carry none of that themselves), and its ONLY lookup path
    (`gb_gen1_locate_rom`, pdna_gen12.c) is an `f_open()` for "<save's own path,
    minus extension>.gb/.gbc" -- an SD-CARD-RELATIVE FatFs open, completely
    independent of the fused-image ROM the box grid's own art reads. PDNA_DELTA has
    no SD card at all, so this lookup ALWAYS fails for a Gen-1 target under this
    harness (G3GB_ERR_NEEDS_BASE -> "NO GEN-1 ROM / Put .gb here..."), before the
    loss screen even renders -- confirmed by actually running this flow against a
    Red-only fused image first and hitting exactly that screen, not an R1 bug.
    Gen 2 needs no base-stats table at all (`gen3_to_gb(..., NULL, ...)` for
    GB_GEN2), so Gold sidesteps the gap entirely and still fully exercises R1's own
    code (the choice/correction logic does not care which Game Boy generation the
    target is).

    Nav: boot picker (row 0 Emerald, row 1 the fused Gen-2 game -- DOWN x1 -> A ->
    S1 info -> A -> box grid, same shape as run_u4_bag()'s own boot-picker
    sequence) -> R x13 (0-based box index 12) to reach the first box with real
    room on Guy's own Gold.sav -- 17/20, confirmed directly against the save's own
    bytes (tests/host_gbsurgery_tool.c --list). R needs a GENEROUS 200-frame
    settle here, not the usual SETTLE/BIG_SETTLE -- confirmed empirically: shorter
    settles intermittently dropped/misregistered a press with no visible sign
    anything was wrong, landing one or more boxes short of the intended one (an
    early draft of this same probe found a near-empty box mid-search purely by
    accident) -> cursor to slot 17 (one of this box's 3 genuinely-empty real
    slots) -> A -> the empty-cell action menu (CREATE / PASTE HERE / CANCEL)
    -> DOWN x1 -> A -> gen3_to_gb() runs against the clip-seeded Charizard L20 ->
    the EXISTING loss screen (unchanged by R1) -> A (proceed) -> the NEW R1 screen
    (gb_paste_legal_screen): "A = KEEP AS IS" / "SELECT = MAKE LEGAL (20 -> 36)" /
    "B = cancel" -> SELECT -> gb_set_level() raises the level, gb_paste_write()
    commits -> back to the box grid with the corrected Charizard sitting in the
    cell -> VIEW it to show LEVEL 36 landed for real, not just claimed by the dialog.

    R1's own scope note: there is no THIRD screen between the choice and the write
    (the design's #3e "Safety" section: the write is verified/backed-up, not
    re-confirmed a second time) -- "the choice dialog" and "the MAKE LEGAL row with
    a concrete level" are the SAME single screen (05 below); there is no separate
    "confirm" screen to shoot distinctly from it -- selecting SELECT commits
    directly, matching the brief's own "additive, no new screen kind" design. The write itself is proven by the host tests (host_gen3gb_test / host_xfer_roundtrip_test); PDNA_DELTA has no SD., not just in dialog text."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "r1_")
    print("== BACKLOG #104 R1: KEEP AS IS / MAKE LEGAL on a Gen 3 -> Game Boy paste ==")

    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # Emerald (row 0) -> Gold (row 1)
    s.tap("A", settle=60)                                   # pick it -> S1 info
    s.tap("A", settle=60)                                   # -> box grid (rom_gbsprite cold fetch)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("01_box_grid", "BACKLOG #104 R1: Gold's box grid, boot-picker -> standalone "
                           "(g_clip pre-seeded with a Charizard at L20 -- fuse_sav.py "
                           "--clip, screenshot-only, no Gen-3 session ever opened). "
                           "BOX1, 20/20 -- no room here, see the R x13 below.")

    # BOX1..BOX12 (0-based box index 0..11) are all 20/20 on Guy's own Gold.sav;
    # BOX13 (0-based index 12) is the first with room -- count=17, slots 17..19
    # genuinely empty within its own 20-slot capacity (confirmed directly: the host
    # surgery tool's --list against the real Gold.sav shows "box 12: count=17,
    # slot 0: dex=241 ... MILTANK", matching what this same R x13 lands on below,
    # species-by-species). R needs a GENEROUS 200-frame settle here, confirmed by a
    # direct probe against known box contents -- shorter settles (BIG_SETTLE=40, or
    # even 80) intermittently dropped presses, landing one or more boxes short with
    # no visible sign anything was wrong (an earlier draft of this same flow found
    # a near-empty box mid-probe purely by accident, not by design).
    s.press_n("R", 13, settle=200)
    # Same drop-prone input path as R above -- DOWN/RIGHT also need a settle well
    # past SETTLE (12 frames): a probe at the default settle landed on slot 13
    # (GYARADOS, still occupied) instead of the intended slot 17, one of THREE
    # RIGHT presses having been swallowed silently. 80 frames, confirmed directly
    # against the known slot contents (slot 16 = LAPRAS, the last occupant; slot
    # 17 = the first empty one), is reliable.
    s.press_n("DOWN", 2, settle=80)
    s.press_n("RIGHT", 5, settle=80)                        # slot 17 -- empty (count=17 here)
    s.shot("02_cursor_on_empty_cell", "BACKLOG #104 R1: BOX13 (R x13 from BOX1, "
                                       "17/20 -- the first box with room), cursor "
                                       "parked on an empty cell before pressing A")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # empty-cell action menu
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> gen3_to_gb() -> loss screen
    s.shot("03_loss_screen", "BACKLOG #104 R1: the EXISTING loss screen, unchanged -- "
                              "what a Gen3->GB transfer drops regardless of which "
                              "choice R1 adds after it")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # proceed -> the NEW R1 screen
    s.shot("04_legal_choice", "BACKLOG #104 R1: the KEEP AS IS / MAKE LEGAL choice, "
                               "a SEPARATE screen (not a row squeezed onto the loss "
                               "screen, which has no room left -- see the layout "
                               "comment in source/pdna_layout.h). Title 'SEND TO "
                               "GAME BOY'; row 'A = KEEP AS IS'; row 'SELECT = MAKE "
                               "LEGAL (20 -> 36)' -- the concrete level this specific "
                               "underlevelled Charizard needs (pk_evo_min_level(6) == "
                               "36); 'B = cancel' below. This is also the shot for "
                               "'the MAKE LEGAL row with a concrete level' -- same "
                               "screen, same row.")

    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # choose MAKE LEGAL -> gb_paste_write()
    s.shot("05_sd_refusal_hardware_only", "BACKLOG #104 R1: SELECT registered cleanly "
                           "(no crash, no corruption) and gb_paste_write() ran -- but "
                           "PDNA_DELTA has NO SD card at all, so f_mkdir(PDNA_SIDECAR_DIR) "
                           "always fails here ('SIDECAR FOLDER / Nothing transferred.') -- "
                           "the SAME refusal ANY Gen3->GB write hits in this build, "
                           "pre-existing and unrelated to R1 (the brief's own words: "
                           "'the delta build's in-session refusal keep today's behaviour "
                           "-- the dialog still shows; the refusal follows as before'). "
                           "The actual WRITE landing with the corrected level (36, not "
                           "20) is proven byte-for-byte by the host test instead -- "
                           "tests/host_gen3gb_test.c section 5 and "
                           "tests/host_xfer_roundtrip_test.c section C -- HARDWARE/HOST-"
                           "ONLY proof, flagged per the standing convention, not faked "
                           "here by pretending this emulator wrote to a card it does "
                           "not have.")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back to the box grid
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # the SAME cell -> still the empty-cell menu
    s.shot("06_cell_still_empty_no_corruption", "BACKLOG #104 R1: the cell is STILL "
                           "the empty-cell action menu (CREATE / PASTE HERE / CANCEL), "
                           "not a half-written mon -- the refused write left nothing "
                           "behind, matching the sidecar-first safety pattern (hard "
                           "rule 3): a write that cannot land refuses cleanly rather "
                           "than partially committing.")
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
    ap.add_argument("--u4-bag", choices=("red", "yellow"),
                     help="U4: only run_u4_bag() against --image for the named game "
                          "(Red's or Yellow's own Item bag) -- --image MUST be a "
                          "ONE-Gen-1-ROM fused image matching this choice (BACKLOG "
                          "#98's fused-image-by-generation-only harness gap)")
    ap.add_argument("--u4-empty", action="store_true",
                     help="N6(e): only run_u4_empty() against --image -- --image "
                          "MUST be fused with a Red save whose Items pocket was "
                          "zeroed by docs/shots/rvu4/mkbag.c (count 0)")
    ap.add_argument("--u5-pack", choices=("gold", "crystal"),
                     help="U5: only run_u5_pack() against --image for the named "
                          "game (Gold's or Crystal's own Pack + PC store) -- "
                          "--image MUST be a ONE-Gen-2-ROM fused image matching "
                          "this choice (BACKLOG #98's fused-image-by-generation-"
                          "only harness gap, same as --u4-bag)")
    ap.add_argument("--d7-gold", action="store_true",
                     help="N6(f): only run_d7_gold() against --image -- --image "
                          "MUST be a Gold-only fused image (Gold.gbc+Gold.sav)")
    ap.add_argument("--r1-xfer", action="store_true",
                     help="BACKLOG #104 R1: only run_r1_xfer() against --image -- "
                          "--image MUST be pokedna-delta-artless.gba fused with an "
                          "ordinary Gen-3 .sav, an 80-byte clip record for an "
                          "underlevelled evolved species (fuse_sav.py --clip), and "
                          "Gold.gbc+Gold.sav (fuse_gb.py, ONE Game Boy ROM -- GOLD, "
                          "not Red: see run_r1_xfer()'s own docstring for why)")
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

    if a.u4_bag:
        try:
            sess = run_u4_bag(core_mod, image_mod, a.image, a.out, a.u4_bag)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u4 bag ({a.u4_bag}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.u5_pack:
        try:
            sess = run_u5_pack(core_mod, image_mod, a.image, a.out, a.u5_pack)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u5 pack ({a.u5_pack}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.u4_empty:
        try:
            sess = run_u4_empty(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u4 empty: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.d7_gold:
        try:
            sess = run_d7_gold(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] d7 gold: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.r1_xfer:
        try:
            sess = run_r1_xfer(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] r1 xfer: {e}")
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
