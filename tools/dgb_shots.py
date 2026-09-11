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


def run_m1_map(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1 (BACKLOG #91, docs/GB-MAP-DESIGN.md): Red's OWN current-map view,
    read-only, on the shared GB-screen shell. `rom` must be a Red-only fused image
    (tools/fuse_gb.py fed only Red.gb+Red.sav -- BACKLOG #98's fused-image-by-
    generation-only gap, same reason run_u4_bag()/run_u5_pack() require a single-
    ROM image).

    Nav: boot picker DOWN -> A -> A -> box grid (rom_gbsprite cold scan) -> START
    -> nav menu -> DOWN x16 (Party->Bank->Daycare->Trainer->Clock fix->Mirage->
    Pokedex->Bag->Flags & counters->Bases->Blocks->Tickets->Records->Frontier->
    Fly->Contests->Map, PDNA_NAV_ITEMS index 16) -> A -> pdna_gbmap_gen1() (gbscr_
    open()'s own cold rom_gbui scan, separate cache from rom_gbsprite's box-grid
    one, PLUS rom_gbmap.c's own separate locate pass over the same ROM).

    m1 review D6/D1 correction: L/R are the shell's own SIZE toggle here (same as
    SELECT) -- the D-pad ALONE pans. wXCoord/wYCoord (Red.sav's real x=6/y=4) are
    ONE halving from a block (gbmap_block_of(), rom_gbmap.h), giving block (3,2),
    NOT (1,2) -- an earlier draft of this file said the player's real x already
    sat at the west clamp (vbx=0); with the corrected halving it does not: width 7
    - the 5-block viewport = 2 steps of slack, and the marker's own block (3) is
    NOT at either edge, so the initial vbx is 1 (clampi(3 - 5//2, 0, 2)), one step
    of room on EACH side."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_")
    print("== M1: Red's own current-map view (boot picker -> standalone -> Map) ==")

    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)                    # Emerald (row 0) -> the GB row (row 1)
    s.tap("A", settle=60)                                    # pick it -> S1 info
    s.tap("A", settle=60)                                    # -> box grid (rom_gbsprite cold fetch)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 16)                                     # Party -> ... -> Map (index 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                    # Map -> pdna_gbmap_gen1()
    s.shot("01_map_1to1", "M1: the player's own current map at 1:1, centred on a "
                           "5x5-block viewport clamped to the map's own bounds "
                           "(no connections/stitching -- that is M2); the red frame "
                           "marks the player's own block (3,2) -- the open floor "
                           "block, roughly centred on the 7-wide map -- NOT the PC "
                           "counter two rows up (D1: block = coord >> 1, one "
                           "halving, not two)")

    s.tap("SEL", settle=60)                                  # shell-wide toggle -> stretched
    s.shot("01b_stretched", "M1: SELECT stretches the same view to 240x160 -- the "
                             "shell's own scale toggle, not a map-specific key")
    s.tap("SEL", settle=60)                                  # back to 1:1
    s.shot("01c_1to1_again", "M1: SELECT again returns to 1:1")

    s.tap("L", settle=60)                                    # D6: L is ALSO the scale toggle here
    s.shot("01d_stretched_via_L", "M1 D6: L is the SAME scale toggle as SELECT on "
                                   "this screen (it does not pan) -- stretched again")
    s.tap("R", settle=60)                                    # R toggles it right back
    s.shot("01e_1to1_via_R", "M1 D6: R toggles it back to 1:1 -- L and R both drive "
                              "the one shared gb_scale_mode, same as SELECT, matching "
                              "the shell-wide 'L/R or SELECT' convention every other "
                              "GB screen uses")

    # VIRIDIAN_POKECENTER (Red.sav's real player map) is 7x4 blocks; the viewport
    # is 5x5 blocks. Vertically the WHOLE map already fits (height 4 <= VBH 5, so
    # vby is pinned at 0 the entire visit: no vertical pan is possible on THIS
    # map, not a bug -- Route 17's own vertical-clamp demo below covers that
    # axis). Horizontally the initial vbx is 1 (see the docstring's own D1
    # correction), one step of slack on EACH side: RIGHT once reaches the east
    # clamp (vbx=2, width 7 - VBW 5), a second RIGHT is a true no-op, then the
    # D-pad's LEFT (not L -- D6) walks it all the way back to the west clamp
    # (vbx=0), two presses, and a third LEFT is a true no-op there too.
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("02_panned_right_to_east_clamp", "M1: RIGHT once reaches the east clamp "
                                             "(vbx=1 -> 2, width 7 - the 5-block "
                                             "viewport = 2) -- the marker is no "
                                             "longer centred, now one block from "
                                             "the viewport's own LEFT edge (its "
                                             "fixed map block stayed put; the "
                                             "viewport panned right past it)")
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("03_east_clamp_no_op", "M1: a second RIGHT from the east clamp is a "
                                   "true no-op -- pixel-identical to the previous "
                                   "shot (vby also never moves off 0 this whole "
                                   "visit: height 4 <= the 5-block viewport)",
           allow_same=True)
    s.tap("LEFT", settle=gb_shots.SETTLE)
    s.shot("04_panned_left_via_dpad", "M1 D6: the D-PAD's own LEFT pans the "
                                       "viewport one block left (vbx=2 -> 1) -- L "
                                       "no longer does this (it is the SIZE "
                                       "toggle, shot 01d above)")
    s.tap("LEFT", settle=gb_shots.SETTLE)
    s.shot("05_west_clamp", "M1: LEFT again reaches the west clamp (vbx=0) -- the "
                             "marker is now near the viewport's own RIGHT edge "
                             "(three blocks from the left), the mirror image of "
                             "shot 02's east-clamp position")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # close -> back to the box grid
    s.shot("06_closed_back_to_grid", "M1: B closes the map screen -- back to the "
                                      "box grid. No save byte is ever written by "
                                      "this read-only screen; the shell's shared "
                                      "SIZE preference (gb_scale_mode, toggled "
                                      "twice above via L/R) still persists to "
                                      "config.cfg on close, exactly as on every "
                                      "other GB screen")

    return s


def run_m1_map_vclamp(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1 (BACKLOG #91) vertical-clamp demo: VIRIDIAN_POKECENTER (Red.sav's real
    starting map) is only 4 blocks tall, shorter than the 5-block viewport, so
    run_m1_map() above can never move vby off 0 -- it has no vertical clamp to
    show. `rom` must be a Red-only fused image built from a save WARPED onto
    Route 17 (map id 28, 10x72 blocks -- tests/host_gbsurgery_tool.c's own
    `--op warp 28 8 68`, landing on block (4, 34): vby's own clamp range is
    [0, 72-5=67], and block 34 sits far enough from BOTH ends that a few UP/DOWN
    presses reach each one without an absurd number of taps.

    Nav: identical to run_m1_map() (same nav menu index, same screen) -- this is
    the SAME pdna_gbmap_gen1() reached from a differently-positioned save."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_vclamp_")
    print("== M1: vertical clamp on Route 17 (warped save) ==")

    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=60)
    s.tap("A", settle=60)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)
    s.shot("01_route17_mid", "M1 vclamp: Route 17 (10x72 blocks), warped to block "
                              "(4,34) -- vby starts at 32 (34 - 5//2), comfortably "
                              "clamped on neither end")

    s.press_n("UP", 32, settle=gb_shots.SETTLE)
    s.shot("02_north_clamp", "M1 vclamp: 32 UPs reach the north clamp (vby=0)")
    s.tap("UP", settle=gb_shots.SETTLE)
    s.shot("03_north_clamp_no_op", "M1 vclamp: one more UP is a true no-op at the "
                                    "north clamp -- pixel-identical to the previous "
                                    "shot", allow_same=True)

    s.press_n("DOWN", 67, settle=gb_shots.SETTLE)
    s.shot("04_south_clamp", "M1 vclamp: 67 DOWNs from the north clamp reach the "
                              "south clamp (vby=67, height 72 - the 5-block "
                              "viewport)")
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_south_clamp_no_op", "M1 vclamp: one more DOWN is a true no-op at "
                                    "the south clamp", allow_same=True)

    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("06_closed", "M1 vclamp: B closes the map screen, same as run_m1_map()")

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
                           "the nameplate label under the picture still reads "
                           "'Items' (reused, not overridden; not independently "
                           "pixel-dumped this slice, see pdna_gbpack.h). D6 "
                           "fix: the description box is the ONE thing that "
                           "changes, now printing 'PC ITEM STORE' -- the "
                           "earlier draft's empty box left the store visually "
                           "identical to the Items pocket")

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
                                       "pocket.' / 'No per-item pocket table "
                                       "yet.' (the dim third line; per-item "
                                       "pocket membership was not located "
                                       "this slice; the brief's own "
                                       "sanctioned fallback is Items-only "
                                       "ADD ITEM, not a silent wrong-pocket "
                                       "accept)")
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
                                "0xEC armed-swap marker is gone (0xED is the "
                                "plain cursor, a different glyph), no entries "
                                "changed")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B with a real pending edit -> commit prompt
    s.shot("12_commit_prompt", "U5: B with a real pending edit -> 'Save pack "
                                "changes?' (app_confirm), the same dialog "
                                "every other GB screen's own commit uses")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline
    s.shot("13_declined", "U5: declining discards the edit -- gbb_write never "
                           "ran, back at the box grid")

    return s


def run_b90_fly(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #90: the Gen-1/2 Fly-destination screen (source/pdna_gbfly.c) over
    gb_fly.h's bitfield core -- the same shape as run_u4_bag()/run_u5_pack() above,
    reused for a plain (non-gbscreen-shell) list screen. `rom` must be a ONE-ROM
    fused image (Red-only for `which == "red"`, Crystal-only for `which ==
    "crystal"` -- same BACKLOG #98 harness-gap reasoning as U4/U5's own images), so
    the boot picker is skipped (gb_delta_pick_save()'s `if (n == 1) return 0`) and
    one A tap reaches S1 info -> box grid directly.

    Nav: A (S1 info) -> box grid -> START -> nav menu -> DOWN x14 (Party=0, Bank=1,
    Daycare=2, Trainer=3, Clock fix=4, Mirage=5, Pokedex=6, Bag=7, Flags&counters=8,
    Bases=9, Blocks=10, Tickets=11, Records=12, Frontier=13, Fly=14 -- PDNA_NAV_ITEMS
    order, source/pdna_layout.h) -> A -> pdna_gb_fly()."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b90_{which}_")
    print(f"== BACKLOG #90: {which}'s own Fly destinations (single-ROM image -> "
          "standalone -> Fly) ==")

    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 14)                                    # Party -> ... -> Fly (index 14)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Fly -> pdna_gb_fly()
    s.shot("01_list", f"BACKLOG #90: {which}'s own Fly destinations -- the "
                       "ON/x count header, one row per gbfy_count() destination, "
                       "cursor on row 1")

    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("02_row2", "BACKLOG #90: DOWN moves the cursor to row 2")

    s.tap("A", settle=gb_shots.SETTLE)                      # toggle the selected row
    s.shot("03_toggled", "BACKLOG #90: A toggles the selected destination -- the "
                          "row's ON/off text and the header count both flip; "
                          "rmbl_fire(RCUE_EDIT) is the same edit haptic every "
                          "other GB screen's own field edits use")

    # D4 (BACKLOG #90): START's mark-all. Fires from the SAME cursor position
    # 03_toggled left the toggled row on, still row 2 -- START asks a confirm, then
    # sets every row not already ON (skipping Gen 2's four spawn-only rows via the
    # "spn" fly_tag() predicate), same shape as the Gen-3 Fly screen's own START.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # Fly's own START (not the box grid's)
    s.shot("04_start_confirm", "BACKLOG #90: START -> 'Mark all destinations? "
                                "Skips story order.' (app_confirm), the same "
                                "one-shot gate pdna_fly.c's own START uses")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # confirm -> mark-all loop -> result panel
    s.shot("05_start_result", "BACKLOG #90: the '%d newly marked.' result panel"
                               + (" -- 'Spawn-only rows untouched.' on Gen 2 (the "
                                  "four spn-tagged rows are never a real Fly-menu "
                                  "destination, so mark-all leaves them exactly as "
                                  "found)" if which == "crystal" else "")
                               + " (the dialog residue behind the panel is the known "
                                 "s_msg-over-app_confirm ghosting, BACKLOG #119 -- present "
                                 "on Gen 3's pdna_fly.c too)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the result panel -> back to the grid

    if which == "crystal":
        # Gen 2 only: scroll to a spawn-only row (index 0/1/17/27 -- HOME/DEBUG/
        # UNION_CAVE/FAST_SHIP, see pdna_gbfly.h's own header note) and show its
        # "spn" tag plus the footer legend explaining it.
        # sel is cyclic (`sel = sel ? sel - 1 : n - 1`): row 2 (idx1) -> UP -> row 0
        # (idx0) -> UP -> WRAPS to the LAST row (idx27, Fast Ship), not back to row 0
        # -- Fast Ship and Union Cave (idx17) are both spawn-only ("spn"-tagged), same
        # as row 0 (Spawn: Home) and row 1 (Spawn: Debug) would have been.
        s.press_n("UP", 2, settle=gb_shots.SETTLE)          # row 2 -> row 0 -> wraps to row 27 (Fast Ship)
        s.shot("06_spawn_only_tag", "BACKLOG #90: row 27 (Fast Ship) carries the "
                                     "'spn' tag and the footer legend 'spn = no effect "
                                     "in game' -- flypoints.asm's own Fly menu "
                                     "never offers this bit as a destination even "
                                     "though wVisitedSpawns has a real bit for it -- "
                                     "still off after D4's mark-all, proving the "
                                     "skip really held")
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)        # back to row 2 (the toggled row)

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B with a real pending edit -> commit prompt
    s.shot("07_commit_prompt", "BACKLOG #90: B with a real pending edit -> 'Save "
                                "fly destinations?' (app_confirm), the same dialog "
                                "every other GB screen's own commit uses")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A = yes -> gb_persist() -> PDNA_DELTA refusal
    s.shot("08_confirmed", "BACKLOG #90: A confirms -- gb_persist()'s PDNA_DELTA "
                            "branch refuses ('Edits are in-session only in the "
                            "emulator build.') because this build has no SD card "
                            "for a GB image at all (same #62 D2/D5 branch every "
                            "other GB screen's own commit hits here); the write "
                            "path itself is proved by the retail gate's fly case "
                            "(tools/gb_retail_gate.py), not this shot")

    return s


# source/osk.c's QWERTY rows (both cases, no shift mode) -- kept as a literal copy
# for the SAME reason g3_shots.py's own osk shot does not need one (it never types
# a full name): this one does, so it needs to know which (row, col) cell holds each
# glyph. Any drift from source/osk.c's own KB[] would only make this driver type
# the wrong letters, never break the app itself -- osk.c stays the single source of
# truth for what actually ships.
_OSK_KB_ROWS = [
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
    " -.,'!?",
]


def _osk_type_char(s: "gb_shots.Session", pos: list[int], ch: str) -> None:
    """Drive the on-screen keyboard's cell cursor from `pos` (mutated in place --
    [row, col], the SAME two ints osk_core's own `cr`/`cc` track) to `ch`'s cell via
    DOWN/RIGHT only (osk.c's cr/cc both wrap, so any cell is reachable without ever
    needing UP/LEFT), then A to insert it at the current caret position. Mirrors
    osk_core's OWN per-loop clamp (`if (cc >= rowlen(cr)) cc = rowlen(cr) - 1;`) when
    a row change lands on a column past the new row's shorter length."""
    target = None
    for r, row in enumerate(_OSK_KB_ROWS):
        c = row.find(ch)
        if c >= 0:
            target = (r, c)
            break
    if target is None:
        raise ValueError(f"_osk_type_char: {ch!r} is not on any osk.c KB row")
    tr, tc = target
    cr, cc = pos
    downs = (tr - cr) % len(_OSK_KB_ROWS)
    if downs:
        s.press_n("DOWN", downs, settle=gb_shots.SETTLE)
    cr = tr
    if cc >= len(_OSK_KB_ROWS[cr]):
        cc = len(_OSK_KB_ROWS[cr]) - 1   # osk_core's own clamp, applied before the next move
    rights = (tc - cc) % len(_OSK_KB_ROWS[cr])
    if rights:
        s.press_n("RIGHT", rights, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.SETTLE)    # KEY_A -> u8w_apply_key(..., U8W_OP_INSERT, KB[cr][tc])
    pos[0], pos[1] = cr, tc


def run_b90_boxname(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """F1 step 3 (BACKLOG #94): the Gen-2 box-rename banner, over gb_boxnames.h's
    gbbn_rename/gbbn_supported (source/pdna_gen12.c's gbsrc_set_name_impl/
    gbsrc_can_rename_impl, the guard-split pair this slice wired). `rom` must be a
    Gen-2-only ONE-ROM fused image (Gold.gbc+Gold.sav or Crystal.gbc+Crystal.sav,
    same BACKLOG #98 harness-gap reasoning as --b90-fly) -- Gen 1 has no box-name
    table at all (gbbn_supported refuses it), so this shot is Gen-2 only by design,
    not a coverage gap.

    Nav: A (S1 info) -> box grid (cur=0, on_title=false) -> UP (on_title=true, the
    banner) -> A -> gbsrc_can_rename_impl() true (g_ed exists on this resident-image
    entry, app_can_edit() true even in the emulator -- only the FINAL write is
    Omega-gated, not the in-RAM edit -- and gbbn_supported() true on a Gen-2 save)
    -> osk_input("BOX NAME", <current name>, ...) opens, seeded and caret-at-end.

    The real sequence differs from a same-shaped Fly/Bag/Pack commit in ONE way this
    slice's own design calls for: gbsrc_set_name_impl() calls gb_persist("boxname")
    itself, directly inside src->set_name() (BEFORE pdna_box.c's own `src->commit()`
    even runs) -- so the PDNA_DELTA refusal's msg_wait (source/pdna_main.c, "Press
    A" to dismiss, NOT B) fires the instant osk_input's own START confirms the new
    name, and the box grid does not repaint with the RENAMED banner until THAT
    dialog is dismissed. Order on screen is therefore: seeded keyboard -> (backspace
    the old name, type a new one, START) -> persist-refusal dialog -> A dismisses it
    -> THEN the renamed banner."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b94_{which}_")
    print(f"== BACKLOG #94: {which}'s own box-rename banner (single-ROM image -> "
          "standalone -> box grid -> banner -> A -> osk_input) ==")

    s.run(700)
    s.tap("A", settle=60)                                    # S1 info -> box grid
    s.run(GB_ART_COLD_SETTLE)
    s.shot("01_box_grid", f"BACKLOG #94: {which}'s own box grid, freshly opened -- "
                           "box 0, cursor on the top-left cell")

    # This transition's own repaint (draw_box_banner's selection frame + draw_footer's
    # on_title hint) lands slower than BIG_SETTLE's 40 frames on this image -- measured
    # empirically (40 frames: still the pre-UP banner/footer; 200: fully repainted) --
    # so it gets its own longer settle rather than a shared constant tuned for the
    # cheaper cursor moves the rest of this file uses BIG_SETTLE for.
    s.tap("UP", settle=200)                                   # cur < COLS -> on_title = true
    s.shot("02_banner_selected", "BACKLOG #94: UP from the top row selects the "
                                  "TITLE row (on_title = true) -- A on the banner "
                                  "is the rename shortcut (BACKLOG #33), same one "
                                  "the Gen-3 box screen already has")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # can_rename true -> osk_input opens
    s.shot("03_osk_seeded", "BACKLOG #94: A on the banner -> gbsrc_can_rename_impl() "
                             "allows it (a live Gen-2 session, an Omega-writable "
                             "cart, gbbn_supported() true) -> osk_input('BOX NAME', "
                             "...) opens seeded with the box's CURRENT name, caret "
                             "already past the last character")

    pos = [0, 0]                                              # osk_core's own cr=0, cc=0 start
    for _ in range(8):                                        # box names cap at 8 chars -- always
        s.tap("B", settle=gb_shots.SETTLE)                    # enough backspaces to clear any seed
    for ch in "TEST":
        _osk_type_char(s, pos, ch)
    s.tap("START", settle=gb_shots.BIG_SETTLE)                # confirm -> set_name() -> gb_persist()
    s.shot("04_persist_refusal", "BACKLOG #94: START confirms the new name 'TEST' "
                                  "-- gbsrc_set_name_impl() writes it via "
                                  "gbbn_rename() THEN calls gb_persist('boxname') "
                                  "itself, so the PDNA_DELTA refusal (source/"
                                  "pdna_main.c's msg_wait, 'Edits are in-session "
                                  "only in the emulator build.', dismissed with A) "
                                  "fires HERE, before the box grid ever repaints -- "
                                  "the write already landed in g_ed->img regardless "
                                  "(same #62 D2 posture every other GB screen's own "
                                  "commit has); the real write is proved by the "
                                  "retail gate's boxname case (tools/"
                                  "gb_retail_gate.py), not this shot")

    s.tap("A", settle=200)                                    # dismiss msg_wait -> box grid repaints
    s.shot("05_renamed_banner", "BACKLOG #94: back at the box grid -- the banner now reads 'GB TEST': gbsrc_get_name() re-reads g_m->g2names (refreshed by gbsrc_set_name_impl() right after the write) and pdna_gen12_box_name() adds the 'GB ' DISPLAY prefix on top; the SAVE holds the undecorated 'TEST' (proved by the retail gate's boxname case and gbbn_read on the corpus). The prefix on renamed Gen-2 boxes is BACKLOG #122.")

    return s


def run_b85_daycare(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #85 (source/pdna_gbdaycare.c): the Gen-1/2 Day-Care screen over
    gb_daycare.h's core -- the Gen-1/2 twin of pdna_main.c's pdna_daycare(). `rom`
    must be a ONE-ROM fused image (tools/fuse_gb.py fed only Red.gb+Red.sav for
    `which="red"`, or only Gold.gbc+Gold.sav for `which="gold"` -- same single-ROM
    requirement run_u4_bag()/run_u5_pack() document, BACKLOG #98's known harness gap).

    RE-SHOT for the DO-NOT-SHIP review's D1 (Put-in moves, not pastes) and D6
    (Take-out lands party-first, else the first box with room, else refuses). The
    original shots entered on BOX1 (index 0, 20/20 on BOTH corpus saves), so every
    Take-out landed straight into GBS_ERR_FULL -- a refusal this file's own caption
    mis-described as the intended behaviour. This run instead enters on the FIRST
    box with a free slot on each corpus save (verified directly with
    tests/host_gbsurgery_tool.c's own --list, matching the review's own measured
    boxes):
      Red.sav   box index 4  (in-game BOX5,  19/20, HITMONLEE at slot 0)
      Gold.sav  box index 12 (in-game BOX13, 17/20, MILTANK   at slot 0)
    so the Take-out SUCCESS path is reachable. R press count == the target box
    index exactly on this fixture (R x4 for Red, R x13 for Gold -- confirmed by
    direct probe, matching this file's own pre-existing "drop-prone input" note
    for R/DOWN/RIGHT: a shorter settle silently swallowed a press once already).
    Both corpus parties are FULL (6/6), so the landing search falls through to
    "first box with room" for both games -- the mon lands back in the SAME box it
    came from (which is exactly right: that box is still the first one with a free
    slot after the Put-in reduced its count by one).

    Nav: S1 info -> A -> box grid (rom_gbsprite cold scan) -> R x(box index) ->
    START -> nav menu -> DOWN x2 (Party->Bank->Daycare, PDNA_NAV_ITEMS index 2) ->
    A -> pdna_gbdaycare(cur_box = the box just selected). The screen itself draws
    no icons (this slice's own header note: the animated yard art is Gen-3-only,
    private to pdna_main.c, out of this file list's reach), so BIG_SETTLE covers
    every transition inside it -- no GB_ART_COLD_SETTLE needed past the box-grid
    entry.

    The egg case is NOT attempted here: producing an `egg_ready` state means
    actually breeding two compatible Pokemon in-game (steps of overworld movement),
    which neither this script nor host_gbsurgery_tool.c's `--op daycare` (deposit
    only) can fabricate without new surgery-tool support outside this slice's file
    list -- flagged as hardware/gameplay-only rather than faked with an edited
    fixture this script cannot itself produce. A genuine "everything is full"
    Take-out refusal is ALSO not reproducible from this real corpus (some box
    always has room once the party is full) -- the refusal shown instead is the
    new D1 confirm-decline path (pressing B at "Send to Day-Care?"), a real,
    reachable, zero-byte-diff refusal this screen did not have before the review."""
    box_index = 4 if which == "red" else 12
    box_label = "BOX5" if which == "red" else "BOX13"
    src_mon = "HITMONLEE" if which == "red" else "MILTANK"
    landed_label = "Box 5" if which == "red" else "Box 13"

    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b85_{which}_")
    print(f"== BACKLOG #85: {which}'s own Day Care (D1/D6 re-shoot) ==")

    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid
    s.run(GB_ART_COLD_SETTLE)
    s.shot("00_box_before", f"BACKLOG #85: {which}.sav's own BOX1 on entry, before "
                             "switching to the box this run enters the Day Care from")

    # R press count == the target box index EXACTLY on this fixture (confirmed by
    # direct probe on BOTH corpus saves). settle=300, not the default BIG_SETTLE=40
    # and not the 250 an earlier revision of this same function used: this exact
    # input (R, held rapid-fire) is the one this codebase has already found
    # drop-prone once (tools/dgb_shots.py's own R1-xfer comment) -- a probe at 250
    # on Gold.sav's own image silently dropped one of the 12 presses needed to
    # reach index 12 (landing on BOX12/20/20, one short and already full), while
    # the SAME 12 presses at 300 landed correctly on BOX13/17/20 every time this
    # was re-checked. 300 also re-confirmed correct on Red.sav.
    s.press_n("R", box_index, settle=300)
    s.shot("01_box_selected", f"BACKLOG #85: {which}.sav's own {box_label} selected -- "
                              f"the first box with a free slot ({src_mon} at slot 0), "
                              "so `cur_box` (what the Day-Care's own Put-in/Take-out "
                              "picker and landing search will use) is this box")

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 2)                                     # Party -> Bank -> Daycare (index 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Daycare -> pdna_gbdaycare()
    s.shot("02_screen", f"BACKLOG #85: {which}'s own Day Care on entry, cur_box = "
                        f"{box_label} -- the panel/menu shape mirrored from "
                        "pdna_daycare(), no yard art")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on slot 0 -> the action popup
    s.shot("03_menu_slot0", "BACKLOG #85: the per-slot action popup on slot 0 (View/Edit + "
                             "Take out if occupied, or Put in if empty, Cancel)")

    # Slot 0 starts empty on both fixtures' own Day Care, so the popup's first row
    # is "Put in" -- A opens cur_box's picker (gbdc_pick).
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Put in -> gbdc_pick over cur_box
    s.shot("04_pick_list", f"BACKLOG #85: the deposit picker -- {box_label}'s own occupied, "
                            "non-Egg slots (gbdc_pick, the smallest 'pick one "
                            "owned mon' list this slice could build, since "
                            "gb_daycare's own gbd_deposit() requires a "
                            "box-shaped record -- see pdna_gbdaycare.h)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick the top row -> D1's new confirm prompt
    s.shot("05_confirm", "BACKLOG #85 D1: the new confirm prompt (app_confirm, "
                          "'Send to Day-Care? / Moves this Pokemon there.') -- "
                          "gbdc_deposit() did not ask before this review; this is "
                          "the same review fix as pdna_main.c's own app_to_daycare()")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # DECLINE -> the D1 refusal/cancel path
    s.shot("06_confirm_declined", "BACKLOG #85 D1: B at the confirm backs out -- gbd_deposit() "
                                   "never ran, gbs_delete() never ran, nothing changed. "
                                   "The only reachable 'refusal' shot in this run: a genuine "
                                   "full-boxes-and-full-party Take-out refusal cannot be "
                                   "built from this real corpus (some box always has room "
                                   "once the party's own 6/6 is accounted for)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on slot 0 again -> Put in again
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # gbdc_pick over cur_box again
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick the top row -> the confirm again
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ACCEPT -> gbd_deposit() + gbs_delete() + gb_persist()
    s.shot("07_persist_notice", "BACKLOG #85: gb_persist()'s own PDNA_DELTA branch: 'Edits "
                                 "are in-session only in the emulator build' -- "
                                 "this build has no SD card, so every write shows "
                                 "this notice and RETURNS FALSE, which is why "
                                 "gbdc_deposit()'s own 'LEFT AT DAY CARE / Moved from "
                                 "the box. Saved.' message (real, shipped text) is "
                                 "never reachable in this build -- not specific to "
                                 "this screen, same gate every other GB write path "
                                 "in this app hits")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the notice -> back at the Day Care screen
    s.shot("08_deposited", f"BACKLOG #85 D1: back at the Day Care screen -- slot 0 now shows "
                            f"{src_mon} (moved, not copied)")

    # settle=120, not BIG_SETTLE=40: a direct probe found the box-count banner still
    # blank at 40 frames after this specific transition (Day-Care -> B -> box grid,
    # a re-entry path no earlier shot list in this file exercised) -- the grid's own
    # mon tiles redraw in time but the "N:GB BOXN cc/20" banner text needs longer.
    s.tap("B", settle=120)                                  # back out to the box grid
    s.shot("09_source_emptied", f"BACKLOG #85 D1: back at {box_label} -- its own count "
                                 "dropped by exactly one and slot 0 no longer shows "
                                 f"{src_mon} (D1's own repro: a Put-in that PASTES "
                                 "instead of MOVES would leave this box unchanged)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu again
    s.press_n("DOWN", 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # back into the Day Care
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the now-occupied slot 0
    s.shot("10_menu_occupied", "BACKLOG #85: the action popup on an OCCUPIED slot -- now "
                                "View/Edit + Take out, no Put in row")
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # View/Edit -> Take out
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Take out -> gbdc_take: withdraw + land + gb_persist()
    s.shot("11_take_out_success", "BACKLOG #85 D6: the SUCCESS case underneath this same "
                                  "generic PDNA_DELTA notice -- both corpus parties are "
                                  "full (6/6), so gbdc_land()'s own search falls through "
                                  f"to the first box with room, which is {box_label} "
                                  "itself (still has a free slot after the Put-in above). "
                                  f"gbdc_take()'s own success text ('Sent to {landed_label}.') "
                                  "is real and shipped but, like the deposit case above, "
                                  "unreachable in this SD-less emulator build -- the STATE "
                                  "change it names is verified by slot 0 emptying (12) and "
                                  f"is hardware-only to see spelled out on screen")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back at the Day Care screen
    s.shot("12_slot0_empty_again", "BACKLOG #85 D6: back at the Day Care screen -- slot 0 is "
                                    "empty again (the Take-out landed the mon back in the "
                                    "box, not a phantom copy left behind)")

    s.tap("B", settle=120)                                  # back out to the box grid (same
                                                              # settle note as 09 above)
    s.shot("12b_box_restored", f"BACKLOG #85 D6: back at {box_label} -- its count is back "
                                f"up to what it was before the Put-in (one more than 09's "
                                f"own shot); gbs_insert() appends at the box's own next "
                                f"free slot, not necessarily slot 0, so {src_mon} is "
                                "back in this box but not necessarily in the same cell -- "
                                "the count is gbdc_land()'s own visual proof, not the "
                                "grid position")

    if which != "red":
        s.tap("START", settle=gb_shots.BIG_SETTLE)          # re-enter for the Gen-2-only leg below
        s.press_n("DOWN", 2)
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        # Back at the main screen, no popup open.
        s.tap("DOWN", settle=gb_shots.SETTLE)               # slot 0 -> slot 1 (the Lady's)
        s.shot("13_slot1_cursor", "BACKLOG #85: Gen 2 only: DOWN moves the row cursor to the "
                                   "second slot -- Gen 1 has no second slot to move to")
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # A on slot 1 -> its own popup
        s.shot("14_menu_slot1", "BACKLOG #85: the same action popup, now targeting slot 1 -- "
                                 "still empty, so Put in only")
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # Put in -> gbdc_pick over cur_box again
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # pick the top row -> the confirm
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # ACCEPT -> deposit + delete -> gb_persist()
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # dismiss the emulator-build notice
        s.shot("15_slot1_deposited", "BACKLOG #85: slot 1 (the Lady's) deposited too -- "
                                      "Man's slot is empty here (it was put in AND taken "
                                      "back out in the D6 demo above), so this shows the "
                                      "one-Pokemon-boarding status text rather than the "
                                      "two-slot compatibility read; that panel wording is "
                                      "unrelated to this pass's D1/D6/D8 fixes and was "
                                      "already exercised by BOTH slots occupied in this "
                                      "same file's earlier BACKLOG #85 shot list")

    return s


def run_b114_yard(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #114: the Day-Care YARD on the Gen-1/2 screen (source/pdna_gbdaycare.c,
    pdna_yard.h) -- dc_scene()/dc_icon_over_bg()/dc_pointer() now draw underneath the
    SAME status panel BACKLOG #85's own shots already covered mechanically (Put in /
    Take out / the confirm prompts); this run is about the SCENE, not the mechanics.

    `rom` must be a ONE-ROM fused image, same requirement as run_b85_daycare (no Gen-3
    ROM is ever registered in a PDNA_DELTA build -- app_register_rom(), the browse-for-
    ROM flow, is `#ifndef PDNA_DELTA` entirely, since there is no SD to browse in the
    emulator; confirmed by direct probe, not assumed). app_yard_visitors_ok() is
    therefore ALWAYS false here (g_yard_visitors also defaults off), so every shot
    below shows the "No visitors: register a Gen-3 ROM" panel row and NO invented
    visitor icons -- the brief's own sanctioned fallback ("visitors present ... else
    the 'no visitors' line"). The Gen-1 visitor cap (<=151, vs Gen 2/3's 251) is
    proven on the host instead (tests/host_yard_test.c), the same posture every other
    "cannot fabricate this state in the emulator" case in this file already takes.

    Real boarder icons ALSO do not render here (mon_icon_for_form_frame's Gen-3-keyed
    icon cache has no source without a registered Gen-3 ROM -- a DIFFERENT lookup path
    than the GB-native rom_gbicon one the box grid already proves works in this same
    fused image, BACKLOG #85's own 00_box_before shot) -- the yard degrades to its
    plain background with no icon, never a blank screen, exactly BACKLOG #114's own
    acceptance line for the artless/no-ROM case.

    Same box-index-to-reach-a-free-slot setup as run_b85_daycare (box 4/HITMONLEE for
    Red, box 12/MILTANK for Gold)."""
    box_index = 4 if which == "red" else 12
    box_label = "BOX5" if which == "red" else "BOX13"

    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b114yard_{which}_")
    print(f"== BACKLOG #114: {which}'s own Day-Care YARD ==")

    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid
    s.run(GB_ART_COLD_SETTLE)
    s.press_n("R", box_index, settle=300)                   # -> the box with a free slot (see run_b85_daycare)

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 2)                                     # Party -> Bank -> Daycare (index 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Daycare -> pdna_gbdaycare()
    s.shot("01_yard_empty", f"#114: {which}'s own Day-Care yard on entry, both slots "
                             "empty -- dc_scene()'s background (the procedural scene "
                             "in this artless image) fills the screen where the two "
                             "text rows used to be; the panel's third row already "
                             "reads 'No visitors: register a Gen-3 ROM' (no ROM is "
                             "ever registered in this emulator build); the footer "
                             f"names the selected empty slot ('{'Boarder' if which == 'red' else 'Man'} "
                             "(empty)') since there is no icon in the yard to point at")

    # NOTE (brief's step 6 asked for a "SELECT scale both ways" shot): checked and
    # dropped -- gb_scale_mode's SELECT toggle (source/pdna_gbscreen.c:157) belongs
    # to the gbscr_run_demo SHELL (the font/card/pack tile-blit viewers reached via
    # Settings' hidden SELECT key, see run_gbscreen_shell() above), which pdna_gbdaycare
    # never routes through -- it draws with ui_*/Mode-3 calls directly and its own key
    # mask has never included KEY_SELECT, before or after this backlog item. Pressing
    # SEL here produced a pixel-IDENTICAL frame (verified: this script's own
    # consecutive-differ check caught it), confirming there is no scale toggle on this
    # screen to demonstrate -- adding one would be a new feature outside BACKLOG #114's
    # six described steps, not a screenshot of existing behaviour.

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the selected empty slot -> the popup
    s.shot("02_popup_over_yard", "#114: the per-slot action popup (gbdc_menu) drawn "
                                  "OVER the yard scene -- the popup geometry is "
                                  "unchanged (PDNA_DCPOP_*), it now sits on top of "
                                  "background art instead of a blank fill")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Put in -> gbdc_pick over cur_box
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick the top row -> the confirm
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ACCEPT -> deposit + delete -> gb_persist()
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the emulator-build notice
    s.shot("03_one_boarder", f"#114: {box_label}'s own mon deposited -- 'Boarding "
                              f"1/{'1' if which == 'red' else '2'}' up top and the "
                              "footer drops '(empty)' now that the selected slot is "
                              "occupied, even though the icon itself does not render "
                              "here (no registered Gen-3 ROM -- see this function's "
                              "own header comment); the yard degrades gracefully, "
                              "it does not go blank")

    if which != "red":
        s.tap("B", settle=120)                              # back to the box grid (same settle
                                                              # note as run_b85_daycare's own re-entries)
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.press_n("DOWN", 2)
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # back into the Day Care
        s.tap("DOWN", settle=gb_shots.SETTLE)               # Man's slot -> Lady's slot
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # A on the Lady's empty slot -> popup
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # Put in -> gbdc_pick over cur_box
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # pick the top row -> the confirm
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # ACCEPT -> deposit + gb_persist()
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # dismiss the emulator-build notice
        s.shot("04_two_boarders", "#114 Gen 2 only: both slots now occupied -- "
                                   "'Boarding 2/2', the panel reads the game's own "
                                   "compatibility flag instead of the single-boarder "
                                   "text, and the footer names whichever slot is "
                                   "currently selected as occupied (no more "
                                   "'(empty)')")

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


def run_b86_clock(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #86/#108: pdna_gbclock.c's own Clock screen -- reached via the standalone
    mount's nav menu, NV_CLOCK (index 4, column 0: Party->Bank->Daycare->Trainer->Clock
    fix, DOWN x4 from a fresh menu, same arithmetic as run_d7_gold's own NV_BAG DOWN x7
    -- see source/pdna_main.c nav_menu(): col = i / PDNA_NAV_ROWS, row = i % PDNA_NAV_ROWS,
    PDNA_NAV_ROWS == 10, so index 4 stays in column 0 and needs no RIGHT press at all).

    `rom` MUST be a Crystal-ONLY fused image (tools/fuse_gb.py fed Crystal.gbc+
    Crystal.sav onto the plain PDNA_TARGET=delta base, same one-ROM-image posture as
    run_d7_gold's own Gold-only image) -- gb_delta_pick_save()'s `if (n == 1) return 0`
    (source/pdna_main.c ~8913) means a single-ROM image skips the boot picker entirely
    and lands straight on the GB save's S1 info page, so this needs exactly ONE tap
    (A: S1 info -> box grid), not DOWN+A+A the way the combined multi-ROM image does.

    Captures: the Clock screen itself (offsets/day-count/weekday/flag readout + the
    three rows), each row's own app_confirm (Ask/Shift/Clear), the shift row's signed
    +-days/hours/minutes editor, and the shift confirm's own dynamic delta line -- B
    declines every confirm except the shift row's (which is driven through to
    gb_persist()'s PDNA_DELTA in-session-only refusal, the same honest outcome every
    other delta-build write ends at, so the shots prove the write PATH runs, not that
    it lands on a card that does not exist in this build)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b86_clock_")
    print("== BACKLOG #86/#108: the Gen-2 Clock screen (Crystal) ==")
    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (single-ROM image)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s.press_n("DOWN", 4)                                       # Party -> ... -> Clock fix (col 0, row 4)
    s.shot("01_nav_menu", "BACKLOG #86/#108: the nav menu with 'Clock fix' selected -- "
                           "NAV_OK on a Gen-2 session now (nav_avail.c's Gen-2 cell), "
                           "column 0 row 4, no RIGHT press needed")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbclock()
    s.shot("02_screen", "BACKLOG #86/#108: pdna_gbclock() itself -- the view-only "
                         "offset/day-count/weekday/flag readout at the top, then the "
                         "three rows (Ask for the time at next load / Shift the clock "
                         "/ Clear the clock-error flag), row 0 selected")

    # Row 0: "Ask for the time at next load" -> its own confirm, declined (B).
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_reset_confirm", "Row 1's own app_confirm -- 'Ask for the time at next "
                                "load?' / 'Asks for the time on the next CONTINUE, "
                                "like a dead battery would.'")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # decline -> back at the screen

    # Row 1: "Shift the clock" -> the signed +-days/hours/minutes editor.
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("04_shift_editor", "Row 2's own editor -- Days/Hours/Minutes steppers, all "
                               "0 on entry, L/R switches field, U/D changes it (BACKLOG "
                               "#86/#108's own signed-delta shape, never an absolute "
                               "time)")
    s.tap("UP", settle=gb_shots.SETTLE)
    s.tap("UP", settle=gb_shots.SETTLE)                       # Days field: 0 -> +2 (nonzero, so A reaches a confirm)
    s.shot("05_shift_editor_dialed", "Row 2's editor with Days dialed to +2 -- the "
                                      "field the confirm below will quote")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # a nonzero delta -> app_confirm
    s.shot("06_shift_confirm", "Row 2's own app_confirm -- 'Shift the clock by this "
                                "much?' / the exact signed delta ('+2d +0h +0m')")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # A = yes -> gbc_shift() -> gb_persist()
    s.shot("07_shift_refusal", "BACKLOG #62 D2/D5's own in-session-only refusal -- "
                                "this delta build has no SD card for gb_persist() to "
                                "write to; the shift itself already landed in RAM "
                                "(pristine re-baselined), only the write-to-card step "
                                "is refused, the same honest outcome every other "
                                "delta-build write in this tree ends at")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # dismiss -> back at the screen

    # Row 2: "Clear the clock-error flag" -> its own confirm, declined (B).
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("08_clear_confirm", "Row 3's own app_confirm -- 'Clear the clock-error "
                                "flag?' / 'Dismisses the banner; a dead battery raises "
                                "it again next boot.'")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # decline -> back at the screen
    return s


def run_b86_clock_gen1_fallback(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #86/#108's own Gen-1 fallback: nav_avail.c keeps NV_CLOCK's Gen-1 cell
    NAV_NOT_IN_GAME (Red/Blue/Yellow never had an RTC at all) -- gb_nav_from_start's
    generic `else if (nv != NV_BACK) app_nav_refuse(nv, kind)` branch handles it, the
    SAME "the row says not in this game" shape run_d7_gold's own NAV_COMING_SOON shot
    already proves for a different row/state. `rom` MUST be a Red-ONLY fused image
    (same single-ROM posture as run_b86_clock's own Crystal-only image, and run_d7_gold's
    Gold-only one) -- ONE tap (A) reaches the box grid, no boot picker."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b86_clock_gen1_")
    print("== BACKLOG #86/#108: the Gen-1 fallback (Red, NAV_NOT_IN_GAME) ==")
    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (single-ROM image)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 4)                                       # same column-0 row 4 as the Gen-2 shot
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # -> app_nav_refuse(NV_CLOCK, SE_KIND_GEN1)
    s.shot("01_not_in_game", "BACKLOG #86/#108: Gen 1 has no clock -- app_nav_refuse() "
                              "shows nav_avail.c's own honest reason ('Gen 1 games have "
                              "no clock.'), never a dead end and never pdna_gbclock() "
                              "itself (gb_nav_from_start's NV_CLOCK branch is gated on "
                              "SE_KIND_GEN2 only)")
    return s


def run_b88_flags(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #88: pdna_gbflags.c's own Flags & counters screen -- reached via the
    standalone mount's nav menu, NV_DATA (index 8, column 0: Party->Bank->Daycare->
    Trainer->Clock fix->Mirage->Pokedex->Bag->Flags & counters, DOWN x8 from a fresh
    menu -- same col=i/PDNA_NAV_ROWS, row=i%PDNA_NAV_ROWS arithmetic run_b86_clock's
    own DOWN x4 uses for NV_CLOCK, index 4).

    `rom` MUST be a ONE-ROM fused image matching `which` ("red" -> Red.gb+Red.sav,
    "crystal" -> Crystal.gbc+Crystal.sav), same single-ROM posture as run_b86_clock/
    run_d7_gold/run_u4_bag.

    Both games share the COUNTERS tab + a toggle + CAUTION + the raw browser + B's
    confirm; they differ in which FLAGS-tab HEADER sits first (Red's own group order,
    tools/gen_gbfields.py's GROUPS_GEN1, starts with "Key events" -- an ordinary
    GBFL_KIND_TOGGLE section, so Red's own toggle/CAUTION shots come from there;
    Crystal's GROUPS_GEN2 starts with "Key items (grant via the Bag)" -- a
    GBFL_KIND_BAG_GRANT section, so Crystal's own run captures the read-only bag-grant
    row instead of toggling anything in it (SELECT jumps to "Key events", the next
    section, TOGGLE-kind, for Crystal's own toggle/CAUTION shots)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b88_flags_{which}_")
    print(f"== BACKLOG #88: the Flags & counters screen ({which}) ==")
    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (single-ROM image)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s.press_n("DOWN", 8)                                       # Party -> ... -> Flags & counters (col 0, row 8)
    s.shot("01_nav_menu", "BACKLOG #88: the nav menu with 'Flags & counters' selected "
                           "-- NAV_OK on both kinds now (nav_avail.c's ok_both row)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbflags(), COUNTERS tab
    s.shot("02_counters_tab", "BACKLOG #88: the COUNTERS tab -- Money/Coins/Rival"
                               + ("/Safari steps" if which == "red" else "/Lucky#")
                               + ", row 0 (Money) selected")

    # A counter edit: Money's own num_entry overlay, then cancel (SELECT) so the
    # underlying value is untouched for the rest of this run.
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_counter_edit", "BACKLOG #88: row 0 (Money)'s own num_entry overlay -- the on-screen "
                               "keyboard/number pad, current value pre-filled")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)              # osk_search: SELECT cancels

    # L/R swap to the FLAGS tab -- every session starts fully collapsed.
    s.tap("R", settle=gb_shots.BIG_SETTLE)
    s.shot("04_flags_tab_folded", "BACKLOG #88: the FLAGS tab, freshly entered -- every section "
                                   "FOLDED (s_gbfl_folded's own 0xFFFFFFFF starting "
                                   "state, mirrors pdna_main.c's own data editor)")

    # Unfold the first header (A on a header row folds/unfolds it).
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("05_section_opened", "BACKLOG #88: header row 0 unfolded ('+' -> '-') -- its member "
                                 "rows are now visible")

    if which == "red":
        # Red's header 0 is "Key events" (GBFL_KIND_TOGGLE) -- step onto its first
        # member row and toggle it: the one-time CAUTION, then the toggled result.
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("06_flag_selected", "BACKLOG #88: the first member row of 'Key events' selected -- "
                                    "a plain GBFL_KIND_TOGGLE row")
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.shot("07_caution", "BACKLOG #88: the one-time CAUTION ('Toggling story flags can / "
                              "soft-lock the save.') -- shown once per screen visit, "
                              "shared between the named list and the raw browser")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # dismiss -> the toggle itself lands
        s.shot("08_toggled", "BACKLOG #88: the flag toggled ON/off -- gbfl_set wrote the bit "
                              "straight into the session's own RAM image")
        jump_presses = 6   # from inside header 0's group: h1,h2,h3,h4,h5,raw
    else:
        # Crystal's header 0 IS "Key items (grant via the Bag)" -- show its read-only
        # row, then SELECT-jump to the next (TOGGLE-kind) section for the toggle/
        # CAUTION shots.
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("06_bag_grant_row", "BACKLOG #88: the first member row of 'Key items (grant via "
                                    "the Bag)' selected -- GBFL_KIND_BAG_GRANT, drawn "
                                    "dim with the '(bag)' suffix")
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.shot("07_bag_grant_message", "BACKLOG #88: A on a bag-grant row -- 'Grant this from the "
                                        "Bag screen, not here.' (no flag is ever "
                                        "toggled by this row)")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # dismiss
        s.tap("SEL", settle=gb_shots.BIG_SETTLE)          # jump to the next header ('Key events'), still FOLDED
        s.tap("A", settle=gb_shots.BIG_SETTLE)            # unfold it (A on a header row folds/unfolds)
        s.tap("DOWN", settle=gb_shots.SETTLE)             # onto its first member row
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.shot("08_caution", "BACKLOG #88: 'Key events' (GBFL_KIND_TOGGLE): the one-time CAUTION "
                              "on its first real toggle")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # dismiss -> the toggle lands
        s.shot("08b_toggled", "BACKLOG #88: the flag toggled -- gbfl_set wrote the bit")
        jump_presses = 5   # from inside header 1's group: h2,h3,h4,h5,raw

    # SELECT-jump around to the trailing raw-browser row, then open it. The jump
    # walks forward through ROW INDICES (not folded-visible positions) to the next
    # header or the raw row.
    for _ in range(jump_presses):
        s.tap("SEL", settle=gb_shots.SETTLE)
    s.shot("09_raw_row_selected", "BACKLOG #88: SELECT-jumped to the trailing 'Raw flag browser "
                                   "(#N)...' row -- mirrors Gen 3's own flags_raw_view "
                                   "entry point")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("10_raw_browser", "BACKLOG #88: the raw flag browser -- every bit of the whole event-"
                              "flags region (2560 Gen 1 / 2048 Gen 2 bits), #N "
                              "centred, ON/off per row")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # back to the FLAGS tab

    # B out of the screen entirely -> the commit confirm (dirty from the toggle above).
    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("11_save_confirm", "BACKLOG #88: B with unsaved edits -- 'Save data changes?' / 'Edits "
                               "write immediately.' (app_confirm, before the one "
                               "gbs_finish()+gb_persist('gbflags') commit)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # decline: this run never actually commits
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
                               "GAME BOY'; D3 review row 'CHARIZARD evolves at L36; "
                               "this one is L20.' first (Charizard's checker floor "
                               "IS its true evolution level, so 'evolves at' is the "
                               "honest wording here); row 'A = KEEP AS IS'; row "
                               "'SELECT = MAKE LEGAL (20 -> 36)' -- the concrete "
                               "level this specific underlevelled Charizard needs "
                               "(pk_evo_min_level(6) == 36); D3 review row 'Either "
                               "way it comes back unchanged.' in dim text; 'B = "
                               "cancel' below. This is also the shot for 'the MAKE "
                               "LEGAL row with a concrete level' -- same screen, "
                               "same row.")

    # D3 review: the B-cancel path the review took directly, not just claimed by the
    # dialog's own "B = cancel" hint text -- pressing B here must return to the box
    # grid with NO SIDECAR dialog (gb_paste_hook's own "B here cancels the whole
    # transfer, nothing written" contract, pdna_gen12.c's own comment on this
    # screen), and the cell must still be empty (the sidecar/gbs_insert/gb_persist
    # sequence never ran). Re-entered immediately after so the SAME probe run also
    # covers the MAKE LEGAL path (05/06 below) -- a screenshot session cannot resolve
    # a single dialog instance two different ways.
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # cancel -> back to the box grid
    s.shot("04b_b_cancel", "BACKLOG #104 R1 review: B on the choice screen cancels "
                            "the WHOLE transfer -- back at the box grid, cell 17 "
                            "still the empty-cell action menu (CREATE / PASTE HERE "
                            "/ CANCEL), no SIDECAR dialog and nothing written. "
                            "Re-entering the same cell below to also exercise MAKE "
                            "LEGAL (05/06) -- a single dialog instance cannot be "
                            "resolved both ways.")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # same empty cell -> action menu again
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> gen3_to_gb() -> loss screen (again)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # proceed -> the R1 screen (again)

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
    ap.add_argument("--b86-clock", choices=("crystal", "red"),
                     help="BACKLOG #86/#108: only run_b86_clock()/"
                          "run_b86_clock_gen1_fallback() against --image for the "
                          "named game -- --image MUST be a ONE-ROM fused image "
                          "matching this choice (Crystal.gbc+Crystal.sav, or "
                          "Red.gb+Red.sav for the Gen-1 fallback shot), same "
                          "single-ROM posture as --d7-gold")
    ap.add_argument("--b85-daycare", choices=("red", "gold"),
                     help="BACKLOG #85: only run_b85_daycare() against --image for "
                          "the named game (Red's one-slot Day Care, or Gold's "
                          "two-slot Day Care + compatibility) -- --image MUST be a "
                          "ONE-ROM fused image matching this choice (same "
                          "single-ROM harness gap as --u4-bag/--u5-pack)")
    ap.add_argument("--b88-flags", choices=("red", "crystal"),
                     help="BACKLOG #88: only run_b88_flags() against --image for the "
                          "named game (pdna_gbflags.c's own Flags & counters screen) "
                          "-- --image MUST be a ONE-ROM fused image matching this "
                          "choice, same single-ROM posture as --b86-clock")
    ap.add_argument("--b114-yard", choices=("red", "gold"),
                     help="BACKLOG #114: only run_b114_yard() against --image for the "
                          "named game (the Day-Care YARD scene, not the D1/D6 "
                          "mechanics --b85-daycare already covers) -- --image MUST be "
                          "a ONE-ROM fused image matching this choice (same "
                          "single-ROM harness gap as --b85-daycare)")
    ap.add_argument("--r1-xfer", action="store_true",
                     help="BACKLOG #104 R1: only run_r1_xfer() against --image -- "
                          "--image MUST be pokedna-delta-artless.gba fused with an "
                          "ordinary Gen-3 .sav, an 80-byte clip record for an "
                          "underlevelled evolved species (fuse_sav.py --clip), and "
                          "Gold.gbc+Gold.sav (fuse_gb.py, ONE Game Boy ROM -- GOLD, "
                          "not Red: see run_r1_xfer()'s own docstring for why)")
    ap.add_argument("--m1-map", action="store_true",
                     help="M1 (BACKLOG #91): only run_m1_map() against --image -- "
                          "--image MUST be a Red-only fused image (Red.gb+Red.sav)")
    ap.add_argument("--m1-map-vclamp", action="store_true",
                     help="M1 (BACKLOG #91) vertical-clamp demo: only "
                          "run_m1_map_vclamp() against --image -- --image MUST be a "
                          "Red-only fused image built from a save WARPED onto Route 17 "
                          "(tests/host_gbsurgery_tool.c's --op warp 28 8 68, see that "
                          "function's own docstring for why)")
    ap.add_argument("--gbmon", action="store_true",
                     help="BACKLOG #92: only run_gbmon() against --image -- the new "
                          "ITEM row on the Gen-2 mon menu. --image MUST be a "
                          "Gen-2-only fused image (Gold.gbc+Gold.sav or "
                          "Crystal.gbc+Crystal.sav, tools/fuse_gb.py, one ROM per "
                          "image -- BACKLOG #98)")
    ap.add_argument("--b90-fly", choices=("red", "crystal"),
                     help="BACKLOG #90: only run_b90_fly() against --image for the "
                          "named game (Red's or Crystal's own Fly-destination "
                          "screen) -- --image MUST be a ONE-ROM fused image "
                          "matching this choice (same BACKLOG #98 harness-gap "
                          "reasoning as --u4-bag/--u5-pack)")
    ap.add_argument("--b94-boxname", choices=("gold", "crystal"),
                     help="F1 step 3 (BACKLOG #94): only run_b90_boxname() against "
                          "--image for the named game (the Gen-2 box-rename banner) "
                          "-- --image MUST be a ONE-ROM fused image matching this "
                          "choice (Gold.gbc+Gold.sav or Crystal.gbc+Crystal.sav), "
                          "same BACKLOG #98 harness-gap reasoning as --b90-fly")
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

    if a.b90_fly:
        try:
            sess = run_b90_fly(core_mod, image_mod, a.image, a.out, a.b90_fly)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b90 fly ({a.b90_fly}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b94_boxname:
        try:
            sess = run_b90_boxname(core_mod, image_mod, a.image, a.out, a.b94_boxname)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b94 boxname ({a.b94_boxname}): {e}")
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

    if a.b86_clock:
        try:
            if a.b86_clock == "crystal":
                sess = run_b86_clock(core_mod, image_mod, a.image, a.out)
            else:
                sess = run_b86_clock_gen1_fallback(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b86 clock ({a.b86_clock}): {e}")
    if a.b85_daycare:
        try:
            sess = run_b85_daycare(core_mod, image_mod, a.image, a.out, a.b85_daycare)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b85 daycare ({a.b85_daycare}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if a.b114_yard:
        try:
            sess = run_b114_yard(core_mod, image_mod, a.image, a.out, a.b114_yard)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b114 yard ({a.b114_yard}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b88_flags:
        try:
            sess = run_b88_flags(core_mod, image_mod, a.image, a.out, a.b88_flags)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b88 flags ({a.b88_flags}): {e}")
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
    if a.m1_map:
        try:
            sess = run_m1_map(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.m1_map_vclamp:
        try:
            sess = run_m1_map_vclamp(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map vclamp: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.gbmon:
        try:
            sess = run_gbmon(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] gbmon: {e}")
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


def run_gbmon(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #92: the new ITEM row on the Gen-1/2 mon-menu popup
    (app_mon_menu_readonly, pdna_main.c) -- Gen 2 only. `rom` must be a Gen-2-only
    fused image (Gold.gbc+Gold.sav or Crystal.gbc+Crystal.sav, tools/fuse_gb.py,
    ONE ROM per image -- BACKLOG #98's fused-image-by-generation harness gap, same
    constraint run_d7_gold()/run_u4_bag() already document).

    A single-ROM fused image has no Emerald/Gen-3 save fused in, so
    gb_delta_boot_pick()'s own `if (n == 1) return 0` (pdna_main.c ~8913) skips the
    boot picker entirely -- exactly run_d7_gold()'s own nav, ONE tap (A: S1 info ->
    box grid), not the DOWN+A+A a combined multi-ROM image needs.

    Guy's own roms/gb corpus has every box on every save completely full (a
    "living dex" test save -- run_standalone()'s own doc comment), so the box
    grid's default cursor position (top-left) is always occupied; no navigation
    is needed before the first A.

    Row order verified here matches BACKLOG #92's brief: VIEW/EDIT, ITEM,
    LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL -- ITEM sits where Gen 3's own
    A_ITEM does (right after the summary row), not where the pre-#92 comment in
    app_mon_menu_readonly (pdna_main.c) said Gen 3 "has no separate Item row" to
    make room for -- that comment is now stale for a Gen-2 mount specifically
    (updated alongside this row, not left to drift)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "gbmon_")
    print("== BACKLOG #92: the Gen-2 mon-menu ITEM row ==")

    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (single-ROM image,
                                                               # no boot picker -- same nav as d7_gold)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("01_box_grid", "#92: box grid, top-left cell occupied (this corpus's "
                           "every box is full) -- the mon this run's ITEM row "
                           "edits")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # occupied cell -> its menu
    s.shot("02_mon_menu", "#92: the Gen-2 mon-menu popup now reads VIEW/EDIT, "
                           "ITEM, LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL -- "
                           "ITEM is the NEW row (AppSrcOps.item / gb_item_hook, "
                           "k_gb_ops_gen2), sitting right after VIEW/EDIT exactly "
                           "where Gen 3's own A_ITEM sits in app_mon_menu's order")

    s.tap("DOWN", settle=gb_shots.SETTLE)                   # VIEW/EDIT (row 0) -> ITEM (row 1)
    s.shot("03_item_row_selected", "#92: cursor on the ITEM row")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> pick_item(), restricted (1..255, "#n")
    s.shot("04_item_picker", "#92: gb_item_hook opens the SAME pick_item() screen "
                              "app_quick_item (Gen 3) uses, restricted to ids "
                              "1..255 shown as \"#n\" via pick_item_set_gen1_2_max "
                              "-- the identical restricted mode gb_editor.c's own "
                              "GBE_ITEM row already uses inside the full summary "
                              "editor, now reachable straight from the mon menu too")

    s.tap("DOWN", settle=gb_shots.SETTLE)                   # move off the current selection
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # confirm -> gb_set_held_item + gb_edit_commit
                                                               # (steps 3-5, the same load/commit path EDIT
                                                               # uses) -> gb_persist()'s PDNA_DELTA branch
    s.shot("05_delta_refusal", "#92: gb_edit_commit's step 5 (gb_persist) hits the "
                                "SAME PDNA_DELTA in-session-only branch run_standalone's "
                                "own D2/D5 shots (06/13) do -- 'Edits are in-session "
                                "only in the emulator build.' The write DID land in "
                                "EWRAM (pristine is re-baselined right here, same as "
                                "D2's fix); nothing is lost, just not persisted to a "
                                "card that does not exist under mGBA")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back at the box grid
    s.shot("06_back_at_grid", "#92: dismissing the refusal returns to the box grid, "
                               "re-paged (g_m->loaded = -1 forces the reload, same "
                               "as every other GB write path)")

    # A short idle run before reopening the menu (past the re-page's own repaint) --
    # without it, a second A right after the dismiss-A landed on the SAME popup
    # again instead of selecting VIEW/EDIT (a settle-timing quirk against this
    # specific state, not a #92/#95 defect -- isolated by hand: the identical two
    # A-taps work first time, straight off the box grid, with no picker/refusal
    # cycle in between).
    s.run(120)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # same cell -> menu again
    s.shot("07_menu_holding_item", "#92: opening the cell's menu again already "
                                    "confirms the write on its own -- the header "
                                    "now reads 'Converted copy / Holding an item' "
                                    "(it read plain 'Converted copy' in shot 02, "
                                    "before ITEM was used) -- gb_item_hook's write "
                                    "landed on the in-EWRAM record, not just on the "
                                    "picker's own display")

    s.run(60)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # VIEW/EDIT -> the native summary
    s.shot("08_summary_item_confirmed", "#92: VIEW/EDIT opens the native GB summary, "
                                         "where the SAME record's Item field (INFO "
                                         "panel) now reads '#2' -- the exact id picked "
                                         "in shot 04 -- confirming the mon-menu row's "
                                         "write landed on the record, not just on the "
                                         "picker's own display")

    # -------------------------------------------------------------------------
    # BACKLOG #95: the summary-field parity audit's closed gaps -- Shiny, Egg, and
    # Met Time/Level/Loc/OT Gender, all new GBE_* rows in gb_editor.c reachable from
    # here via SELECT (pdna_gbedit.c's flat field-list editor, the reviewed
    # fallback BACKLOG #41 already documents).
    # -------------------------------------------------------------------------
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # summary -> the flat editor
    # A generous idle pad before the first input on this screen -- without it, the
    # first DOWN (and only the first) silently did not move the cursor, a timing
    # quirk against this specific "picker -> refusal -> re-page -> menu -> summary
    # -> SELECT" run of screens (isolated by hand: the SAME editor, reached by a
    # plain two-tap A,A + SELECT off a fresh box grid, takes ordinary SETTLE-length
    # DOWNs with no pad needed at all -- see this file's own git history for the
    # isolation). Not a #92/#95 defect; every DOWN below gets the same larger pad
    # to stay safely inside whatever margin actually fixed it.
    s.run(200)
    s.shot("09_flat_editor_top", "#95: pdna_gbedit.c's flat field-list editor over "
                                  "the SAME record -- gbe_fields() drives every row "
                                  "generically, so the six new rows this slice adds "
                                  "appear here with zero screen-side code")

    # ---------------------------------------------------------------------------
    # gbmon C4 reshoot: the Mail confirm. ITEM is row 4 (NICK=0,OT=1,OTID=2,LEVEL=3,
    # ITEM=4) -- a short detour off row 0, back to row 0 before the 29-DOWN scroll
    # below (which is calibrated to start there). item_build()'s restricted-mode
    # search is a NUMERIC PREFIX match (source/pdna_pick.c, item_build's own
    # comment), so typing "158" through osk_search finds item id 158 (G2_MAIL_FLOWER,
    # source/gb_session.c) directly rather than paging one id at a time. Declined
    # (B = no, source/pdna_main.c's app_confirm) so the record's held item stays at
    # #2 (set back in shot 05) -- an ACCEPT here would zero out gb_set_egg's own
    # "no item" precondition for the later Egg-refusal shot below, which depends on
    # the item staying non-zero.
    # ---------------------------------------------------------------------------
    s.press_n("DOWN", 4, settle=60)                         # NICK (0) -> ITEM (4)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> pick_item(), current = #2
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # -> osk_search
    s.tap("A", settle=gb_shots.SETTLE)                      # type '1' (row0 col0, no seed to clear)
    s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)           # col0 '1' -> col4 '5'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '5' -> field "15"
    s.press_n("RIGHT", 3, settle=gb_shots.SETTLE)           # col4 '5' -> col7 '8'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '8' -> field "158"
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm search -> filtered to id 158
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # select id 158 (G2_MAIL_FLOWER) ->
                                                              # gbs_is_mail_item(158) true, no Egg yet
                                                              # -> app_confirm(PDNA_GBEDIT_MAIL_TITLE)
    s.shot("mail_confirm", "#95: gbmon C4 reshoot: picking a Mail id (158, G2_MAIL_FLOWER) "
                            "on the ITEM row triggers app_confirm(PDNA_GBEDIT_MAIL_TITLE, "
                            "PDNA_GBEDIT_MAIL_L1) -- 'SET THIS MAIL ITEM? / No mailbox: "
                            "locks Move/Release.' -- since this tree tracks no mailbox "
                            "(gbs_is_mail_item, gb_session.h)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline -> item unchanged (#2)
    s.shot("mail_declined", "#95: gbmon C4 reshoot: B declines -- back on the ITEM row, "
                             "unchanged (still '#2' from shot 05); a declined confirm "
                             "must not silently set the item anyway")
    s.press_n("UP", 4, settle=60)                           # ITEM (4) -> NICK (0), back where
                                                              # the 29-DOWN scroll below expects
                                                              # to start

    # NICK,OT,OTID,LEVEL,ITEM,FRIEND (6) + MV0-3 (4) + PPU0-3 (4) + PP0-3 (4) +
    # DVA,DVD,DVS,DVC,DVH (5) + GENDER (1, Bulbasaur has a real gender ratio) = 24
    # rows before SHINY -- 29 DOWNs lands on Met OT Gender (row 29), scrolling the
    # 16-row window to show DVA..Met OT Gender (rows 14-29) in one screen: every
    # new row from this slice, in the SAME shot. Taken in THREE chunks, each ending
    # in a real shot() call -- one long chunk of 29 raw taps with no shot() in
    # between reliably lands back on row 0 against this exact multi-screen run (a
    # harness quirk isolated by hand: a shot() call between chunks fixes it every
    # time, on this same image, same history, same everything else -- not a
    # #92/#95 product defect; the flat editor's own row model is independently
    # proven by host_gbeditor_test.c's 461,230 checks over real saves).
    s.press_n("DOWN", 10, settle=60)
    s.shot("10a_scrolling", "#95: 10 DOWNs in -- cursor on Max PP 1, still well "
                             "above the new rows")
    s.press_n("DOWN", 10, settle=60)
    s.shot("10b_scrolling", "#95: 20 DOWNs in -- cursor on DV Spe, Gender/Shiny/Egg/"
                             "Met just a few rows further down")
    # gbmon C11 reshoot: the four Met rows (gb_editor.c's gbe_fields) are gated on
    # e->has_caught, which gb_mark_caught (source/pdna_gen12.c) only ever sets true
    # for a Crystal session (gb_session_is_crystal) -- Gold/Silver never gets them.
    # "gold" appears in every Gold-only fused image this script builds and nowhere
    # in a Crystal one (crystal_only.gba / crystal_forced.gba), so this is an exact
    # discriminator for THIS harness's own naming convention, not a guess.
    is_crystal = "gold" not in rom.name.lower()
    s.press_n("DOWN", 9, settle=60)
    if is_crystal:
        s.shot("10_new_rows_visible", "#95: C11 reshoot: 29 DOWNs in -- Shiny and Egg "
                                       "both 'No', and the four Met fields VISIBLE, "
                                       "carrying this real Crystal.sav mon's own "
                                       "capture record (Met Time 'Morning', a level, "
                                       "Met Loc '#16', Met OT Gender 'M') -- decoded, "
                                       "not invented, sitting right where Gen 3's own "
                                       "F_SHINY and F_MET* rows sit in pdna_edit.c's "
                                       "row order. gb_mark_caught set has_caught=true "
                                       "for this Crystal session (gb_session_is_crystal)")
    else:
        s.shot("10_new_rows_visible", "#95: C11 reshoot: 29 DOWNs in on a GOLD-only image -- "
                                       "count the rows: Shiny, Egg, then StatExp HP/"
                                       "Atk/Def/Spe -- the four Met rows are ABSENT. "
                                       "gb_mark_caught (source/pdna_gen12.c) never sets "
                                       "has_caught true for a Gold/Silver session "
                                       "(gb_session_is_crystal returns false), and "
                                       "gbe_fields() (gb_editor.c) skips GBE_METTIME/"
                                       "METLEVEL/METLOC/METOTGENDER outright when "
                                       "!e->has_caught -- the C11 fix's own point: a "
                                       "Gen-2 target no longer gets a capture record "
                                       "just because it is Gen 2")

    # This fused image's top-left cell may or may not be the Atk-DV-forced-female
    # Bulbasaur (crystal_forced.gba only, built specifically so this species' 7:1-
    # male ratio has NO shiny candidate in its current gender -- see gbmon's own
    # BACKLOG #95 item 4 brief). Every OTHER fused image (Gold, the plain Crystal
    # corpus) has this same box-0-slot-0 Bulbasaur at its REAL corpus gender
    # (already male in both, so Shiny ON keeps it male trivially -- no popup at
    # all): detect by filename, per this file's own d7_gold precedent, rather
    # than probing the frame, so the two paths cannot silently diverge on a typo.
    forced_gender_image = "forced" in rom.name.lower()
    s.press_n("UP", 5, settle=60)                           # Met OT Gender (29) -> Shiny (24)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle ON
    if forced_gender_image:
        s.shot("11_shiny_on", "#95: gbmon C3 reshoot: this fused image's top-left cell is "
                               "Bulbasaur with Atk DV forced to 1 (gender ratio 31, 7:1 "
                               "male) -- currently FEMALE, and none of the 8 shiny Atk-DV "
                               "candidates {2,3,6,7,10,11,14,15} is in the female range "
                               "(dv<=1) at that ratio, so A on Shiny is FORCED to move "
                               "gender: gbe_flip_shiny() sets shiny_gender_forced and "
                               "pdna_gbedit.c's gbedit_shiny_forced_note() pops the "
                               "PDNA_GBEDIT_SHINY_FORCED_TITLE/MALE_L1/L2 message "
                               "immediately ('GENDER FORCED / No shiny female exists for "
                               "this species; it is now male.') -- gb_editor.c's own C2 "
                               "case, now shown live instead of only host-tested")
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # dismiss the forced-gender popup
        s.shot("11b_shiny_on_dismissed", "#95: gbmon C3 reshoot: A dismisses the popup, back "
                                          "on the flat editor -- Shiny now reads 'Yes' and "
                                          "the Gender row above it (not shown in this crop) "
                                          "now reads male, matching the forced result")
    else:
        s.shot("11_shiny_on", "#95: A on Shiny flips it to 'Yes' -- this corpus mon is "
                               "already male and a male shiny Atk-DV candidate exists "
                               "(gbe_flip_shiny() keeps the current gender when it can), "
                               "so no forced-gender popup fires here -- see the gbmon "
                               "C3 reshoot's own crystal_forced image for that case, "
                               "built with Atk DV set to 1 specifically so no shiny "
                               "candidate shares this 7:1-male species' female gender")

    s.tap("DOWN", settle=60)                                # Shiny -> Egg
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle ON -- refused: this record
                                                              # already holds item #2 (shot 05)
    s.shot("12_egg_refused", "#95: gbmon C10 reshoot: A on Egg is REFUSED, not silently "
                              "ignored -- this record already holds item #2 (planted "
                              "back in shot 05 via the mon-menu ITEM row), and "
                              "gb_set_egg(e, true) refuses outright while the held-item "
                              "field is non-zero (review C5, gb_edit.c). C10 wires the "
                              "refusal to a real message: gbedit_adjust_refused's "
                              "GBE_EGG case pops PDNA_GBEDIT_EGG_ITEM_TITLE/L1 -- 'EGG "
                              "CAN'T HOLD ITEMS / Remove the held item first.' -- the "
                              "SAME message pdna_gbedit.c's GBE_K_ITEM branch shows for "
                              "the other direction (a non-zero item picked while the "
                              "record IS an Egg), reused rather than duplicated")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -- Egg never moved,
                                                              # cursor still on the Egg row
    s.shot("12b_egg_refused_dismissed", "#95: gbmon C10 reshoot: A dismisses the message; "
                                         "back on the flat editor with Egg still 'No' -- "
                                         "the refusal left the record untouched, exactly "
                                         "like every other GBE_K_NUM refusal in this row")

    if is_crystal:
        s.tap("DOWN", settle=60)                            # Egg -> Met Time
        s.tap("RIGHT", settle=60)                           # None -> Morning
        s.tap("RIGHT", settle=60)                           # Morning -> Day
        s.tap("RIGHT", settle=60)                           # Day -> Night
        s.tap("DOWN", settle=60)                            # Met Time -> Met Level
        s.press_n("RIGHT", 5, settle=60)                    # Met Level 0 -> 5
        s.shot("13_met_edited", "#95: Met Time cycled to 'Night' and Met Level bumped "
                                 "by +5 (RIGHT x5) to whatever this real Crystal.sav "
                                 "record's own original level was + 5 -- gb_set_caught() "
                                 "repacking bytes 0x1D/0x1E one field at a time via the "
                                 "new gb_get_caught_* readers, the same 'read the other "
                                 "three, write all four back' shape Gen 3's own "
                                 "F_METLEVEL/F_METGAME rows use over metLocation/metGame")
    else:
        # No Met rows exist to edit on Gold -- the row right after Egg is StatExp HP
        # (gbe_fields()'s next entry once METTIME..METOTGENDER are skipped). Edited
        # anyway (same DOWN/RIGHT shape) so this run still ends on a real edited-field
        # shot rather than stopping short, and to keep this function's tap count
        # identical between the two branches (only the CAPTION differs, matching what
        # is actually on screen -- see the C11 reshoot comment above shot 10).
        s.tap("DOWN", settle=60)                            # Egg -> StatExp HP
        s.press_n("RIGHT", 3, settle=60)
        s.tap("DOWN", settle=60)                            # StatExp HP -> StatExp Atk
        s.press_n("RIGHT", 5, settle=60)
        s.shot("13_statexp_edited", "#95: C11 reshoot: with no Met rows to land on, the "
                                     "same DOWN/RIGHT taps instead land on StatExp HP "
                                     "then StatExp Atk (gbe_fields()'s next entries "
                                     "after Egg on a Gold session) -- proof the row "
                                     "list genuinely reflows around the absent Met "
                                     "rows rather than leaving a gap or a stale cursor")
    return s


if __name__ == "__main__":
    sys.exit(main())
