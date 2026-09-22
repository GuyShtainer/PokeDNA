#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""g3_shots.py — headless mGBA screenshots for BACKLOG #15 (rename OSK) and
BACKLOG #2 (collapsible flag sections + the honest Elite-Four section).

Reuses tools/gb_shots.py's Session/load_mgba (imported, not copied or edited —
lane g1 owns that file) and the SAME fuse-a-Gen-3-save path run_e4_settings()
uses: a NORMAL Emerald .sav fused onto pokedna-delta.gba (tools/fuse_sav.py,
no --gb), which boots straight to the box screen — no GB fork involved.

WHAT THIS DRIVES
----------------
#15 (source/osk.c / BACKLOG #15, "truncates it and the rest is lost, instead
of letting me edit from the end"): the file browser itself needs an SD card
mGBA does not have, so this captures the SAME osk_input() call through a
reachable caller instead — the box-name rename, which is source/pdna_box.c's
"A on the banner renames the box directly" path (BACKLOG #33): box screen ->
UP (grid row 0 -> the title row) -> A -> the "BOX NAME" keyboard, seeded with
the box's CURRENT name and the caret already at its end (osk_core's `cpos =
len` — see tests/host_osk_test.c's test_seed_full_and_caret_at_end for the
byte-exact proof this same seed/caret logic holds for a name far longer than
any box name can be). This shot is evidence the mechanism is wired up on a
real screen; the exact "40 glyphs survive, caret lands on glyph 40" claim is
proven on the host, not by eye, because box names cap at 8 characters and a
real long FAT filename is hardware-only (no SD in the emulator).

#2 (source/flags_fold.c + pdna_main.c's data_editor_tab / BACKLOG #2): box
screen -> START (nav menu) -> DOWN x8 (Party -> ... -> NV_DATA, "Flags &
counters" — see source/pdna_layout.h's PDNA_NAV_ITEMS X-macro for the order)
-> A (data_editor_tab, opens on tab 0 COUNTERS) -> R (COUNTERS -> FLAGS, tab
2). Three shots: the tab as first entered this session (every section
collapsed — s_flags_folded's own default, 0xFFFFFFFF); one section expanded
(A on "Badges", the first header); and — the actual BACKLOG #2b payload —
the "Elite Four (reset @HoF)" section expanded, showing the honest section
title plus the achievement flags tools/gen_data.py (commit 1b85e3e) added:
"League beaten (HoF)" (FLAG_SYS_GAME_CLEAR) next to the four per-member E4
flags. The fused save is Guy's own Emerald.sav (roms/, read-only — fuse_sav.py
only ever APPENDS a copy, never mutates the source), so these are his real
values: tests/host_data_test.c's section (5) already proved League beaten is
ON and all four per-member flags read off in this exact file — this shot is
that same evidence, on screen.

Usage:
    /usr/local/bin/python3 tools/g3_shots.py --emerald FUSED_EMERALD.gba \
        --out docs/shots/gb

Frame-advance idiom: identical to gb_shots.py (see its own docstring) — every
screen here also runs with d-pad auto-repeat enabled, so every tap is a short
hold + a settle, longer after anything that opens/closes a whole screen.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))   # so `import gb_shots` finds its sibling

from gb_shots import Session, load_mgba, BIG_SETTLE, SETTLE   # noqa: E402  (Session helpers only — gb_shots.py itself is untouched)


def run_osk_rename(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #15: box-name rename OSK, seeded with the box's real current name."""
    s = Session(core_mod, image_mod, rom, out_dir, "b2osk_")
    print("== BACKLOG #15: box-name rename OSK (osk_input, pdna_box.c) ==")

    s.tap("UP", settle=BIG_SETTLE)         # grid row 0 -> the title row (on_title = true)
    s.tap("A", settle=BIG_SETTLE)          # title row, A -> osk_input("BOX NAME", <current name>, ...)
    s.shot("01_rename_seeded", "#15: box-name RENAME (osk_input) opened on the CURRENT name — "
                                "seeded in full, caret already past the last character (the "
                                "highlighted cell after the text), not truncated and not reset "
                                "to the front. Box names cap at 8 chars (game limit, unaffected "
                                "by this fix); a real FAT long filename needs the file browser, "
                                "which has no SD card in the emulator — the exact 40-glyph "
                                "seed/caret-at-end claim is proven byte-for-byte on the host "
                                "instead (tests/host_osk_test.c, test_seed_full_and_caret_at_end)")

    s.tap("B", settle=BIG_SETTLE)          # cancel — do not actually rename anything
    return s


def run_flags_sections(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #2a/#2b: the FLAGS tab's collapsible sections, and the honest
    Elite-Four section with the achievement flags."""
    s = Session(core_mod, image_mod, rom, out_dir, "b2flags_")
    print("== BACKLOG #2: FLAGS tab — collapsible sections + the Elite-Four truth ==")

    s.tap("START", settle=BIG_SETTLE)      # box screen -> nav menu
    # Party(0) -> Bank -> Daycare -> Trainer -> Clock fix -> Mirage -> Pokedex -> Bag ->
    # Flags & counters(8): all within nav column 0 (rows 0..9), no RIGHT needed —
    # source/pdna_layout.h PDNA_NAV_ITEMS is the one place this order is defined.
    s.press_n("DOWN", 8)
    s.tap("A", settle=BIG_SETTLE)          # NV_DATA -> data_editor_tab(-1), opens on tab 0 COUNTERS
    s.tap("R", settle=BIG_SETTLE)          # COUNTERS -> FLAGS (tab 2; L/R toggles the pair)

    s.shot("01_all_collapsed", "#2: FLAGS tab, first entered this session — every section "
                                "collapsed by default (s_flags_folded == 0xFFFFFFFF), each "
                                "header prefixed \"+\" (BACKLOG #2a)")

    s.tap("A", settle=BIG_SETTLE)          # sel==0 is the first header ("Badges") -> fold-toggle
    s.shot("02_one_section_expanded", "#2: A on a header (\"Badges\") unfolds just that "
                                       "section — marker flips to \"-\", its rows appear, "
                                       "cursor movement now walks into them (BACKLOG #2a)")

    s.tap("A", settle=BIG_SETTLE)          # re-fold Badges, so only Elite Four is open below
    # SELECT jumps to the next header in ABSOLUTE row order regardless of fold state
    # (pdna_main.c's own KEY_SELECT case) — Badges -> Fly destinations -> System -> Gyms
    # -> Elite Four (reset @HoF): 4 jumps, per source/data_tables.c's generated order.
    s.press_n("SEL", 4, settle=SETTLE)
    s.tap("A", settle=BIG_SETTLE)          # expand "Elite Four (reset @HoF)"
    s.shot("03_elite_four_honest", "#2: the \"Elite Four (reset @HoF)\" section, expanded — "
                                    "the honest annotation (RSE clears the four per-member "
                                    "flags after the Hall of Fame so the E4 can be "
                                    "rechallenged: pokeemerald/pokeruby hall_of_fame.inc "
                                    "ResetEliteFour) plus \"League beaten (HoF)\" "
                                    "(FLAG_SYS_GAME_CLEAR), the flag that actually records "
                                    "the win (BACKLOG #2b, tools/gen_data.py 1b85e3e). This "
                                    "is Guy's real Emerald.sav: League beaten reads ON, all "
                                    "four per-member flags read off — the same evidence "
                                    "tests/host_data_test.c section (5) prints on the host")

    s.tap("B", settle=BIG_SETTLE)          # leave the FLAGS tab (no edits made -> no confirm)
    return s


def run_daycare(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #114's own pixel-parity proof: pdna_daycare() (source/pdna_main.c),
    the Gen-3 Day-Care yard scene, BEFORE and AFTER the yard's extraction into
    source/pdna_yard.{c,h}. Not itself a feature shot -- run this exact script
    against a pre-extraction build and a post-extraction build of the SAME fused
    Emerald save and `cmp` the two 01_yard PNGs byte-for-byte: identical, or the
    extraction changed real behaviour (STOP per the brief).

    Nav: box screen -> START (nav menu) -> DOWN x2 (Party(0) -> Bank(1) ->
    Daycare(2), PDNA_NAV_ITEMS order, same idiom every other g3_shots.py run_*
    uses) -> A (pdna_daycare()). One shot only -- the yard's own idle-bob timer
    and per-visit RNG (dc_seed: a session counter + the cart RTC, absent in
    mGBA) are otherwise deterministic across two runs of this same script
    against two builds that only differ in which .c file owns the code, so a
    single frame is enough for the cmp; no keys are pressed inside the screen
    that could let the two runs diverge on timing."""
    s = Session(core_mod, image_mod, rom, out_dir, "b114yard_")
    print("== BACKLOG #114: Day-Care yard scene (pixel-parity proof) ==")

    s.tap("START", settle=BIG_SETTLE)      # box screen -> nav menu
    s.press_n("DOWN", 2)                   # Party(0) -> Bank(1) -> Daycare(2)
    s.tap("A", settle=BIG_SETTLE)          # NV_DAYCARE -> pdna_daycare()
    s.shot("01_yard", "#114: the Gen-3 Day-Care yard scene -- the pixel-parity "
                       "reference this same shot must cmp byte-identical against "
                       "before and after the yard's extraction into pdna_yard.c")

    s.tap("B", settle=BIG_SETTLE)          # leave Day-Care -> box screen
    return s


def run_contests(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #60: the CONTESTS nav row, the museum painting list, the donor picker
    (party -> mon), the write confirm, and the summary's own RIBBONS card after a
    Master-rank ribbon edit on the same donor.

    Nav order: source/pdna_layout.h's PDNA_NAV_ITEMS puts Contests at index 15
    (Party=0 .. Fly=14, Contests=15) -- DOWN x15 from the menu's default Party
    selection lands there directly (nav_menu's DOWN walks the full enum order,
    wrapping columns, so a column-relative count would be wrong)."""
    s = Session(core_mod, image_mod, rom, out_dir, "b60_")
    print("== BACKLOG #60: Contests (museum paintings + donor picker + ribbons) ==")

    s.tap("START", settle=BIG_SETTLE)      # box screen -> nav menu
    s.press_n("DOWN", 15)                  # Party(0) .. Fly(14) -> Contests(15)
    s.shot("01_nav_row", "#60: the START menu's new \"Contests\" row, same panel/format "
                          "as every other row (BACKLOG #58 parity)")

    s.tap("A", settle=BIG_SETTLE)          # -> pdna_contest, museum page, sel=0 (Cool)
    s.shot("02_museum_list", "#60: the 5 Lilycove Art Museum painting slots -- category, "
                              "current species (or \"(empty)\"), and rank")

    s.tap("R", settle=BIG_SETTLE)          # L/R flips page: MUSEUM -> HALL
    s.shot("02b_hall", "#60: the Contest Hall page on Guy's own Emerald save -- 6 "
                        "populated rows; the D2 fix is visible here (slot 3, Tough/"
                        "Plusle, reads Master -- with the old RANK_NAME table indexed "
                        "by the ribbon-word scale instead of CONTEST_RANK_*, this same "
                        "byte (3) would have displayed as \"Hyper\")")
    s.tap("L", settle=BIG_SETTLE)          # HALL -> back to MUSEUM, sel still 0 (Cool)

    s.tap("A", settle=BIG_SETTLE)          # A on Cool -> donor picker, source list
    s.shot("03_picker_source", "#60: donor picker, level 1 -- Party, then the 14 PC boxes")

    s.tap("A", settle=BIG_SETTLE)          # A on Party -> mon list
    s.shot("04_picker_mons", "#60: donor picker, level 2 -- the party's occupied slots "
                              "(species + nickname)")

    s.tap("A", settle=BIG_SETTLE)          # A on the first party mon -> confirm dialog
    s.shot("05_confirm", "#60: the write confirm -- names the donor and the category "
                          "before touching the save (verified-write pipeline, hard rule 3)")

    s.tap("A", settle=400)                 # A -> gc_museum_set + app_commit_sb1: a real flash
                                            # write (flashsave.c byte-by-byte toggle-bit polling
                                            # over up to 4 sectors) + a 12-vsync grow_in flourish
                                            # -- empirically ~150-250 frames wall time in mGBA,
                                            # nowhere near landed by BIG_SETTLE (40) alone
    s.tap("A", settle=BIG_SETTLE)          # dismiss the "SAVED / Flash written" msg_wait
    s.shot("06_museum_written", "#60: back on the museum list -- the Cool painting now "
                                 "shows the donor's species and Master rank")

    # D1 regression: the SAME donor, a DIFFERENT category, on the confirm line that
    # used to overflow l1[32] for a 10-char species + Beauty ("Show TYRANITAR as
    # Beauty winner?" is 33 B including the NUL; l1 is 48 B now).
    s.press_n("DOWN", 1)                   # Cool(0) -> Beauty(1)
    s.tap("A", settle=BIG_SETTLE)          # -> donor picker, source list
    s.tap("A", settle=BIG_SETTLE)          # Party -> mon list
    s.tap("A", settle=BIG_SETTLE)          # first party mon -> confirm dialog
    s.shot("05b_confirm_beauty", "#60/D1: the write confirm for Beauty -- the exact "
                                  "species+category combination the review found "
                                  "overflowing l1[32]; fits comfortably in l1[48] now")
    s.tap("A", settle=400)                 # write it (same flash-write path as Cool above)
    s.tap("A", settle=BIG_SETTLE)          # dismiss the "SAVED" msg_wait

    s.tap("B", settle=BIG_SETTLE)          # leave Contests -> box screen

    # Open a mon's summary and set + show its own RIBBONS card. The museum write above
    # is a SEPARATE SaveBlock1 record (the painting) -- it does NOT touch any mon's own
    # ribbons word (gen3_contest.h's header explains why: (a) per-mon ribbons and (b)
    # the museum record are deliberately independent data), so this is its own edit,
    # showing the OTHER half of BACKLOG #60. Re-entering pdna_box() after Contests
    # resets s_cur_mode to CM_NORMAL and the cursor to the grid's cell 0 (pdna_box.c)
    # -- SEL there CYCLES cursor mode (NORMAL->MOVE->ITEM), it does NOT switch to
    # party (that is the top tabs' PARTY entry, a nested overlay popup) -- so this
    # flow uses the box grid's own cell 0 directly, plain A, no SEL.
    s.tap("A", settle=BIG_SETTLE)          # cell 0 (CM_NORMAL) -> mon menu (View/Edit first)
    s.tap("A", settle=BIG_SETTLE)          # View/Edit -> summary opens in VIEW mode, card 0 (INFO)
    s.press_n("R", 8, settle=SETTLE)       # INFO(0)..CONDITION(7) -> RIBBONS(8) -- L/R flips
                                            # cards in VIEW mode too, so this works before editing
    s.tap("A", settle=BIG_SETTLE)          # VIEW mode: A enters EDIT (pdna_summary.c's
                                            # documented "Summary opens in VIEW, A inside enters
                                            # edit" -- same key, does NOT touch a field yet
    s.shot("07_ribbons_view", "#60: the summary's new RIBBONS card, entering EDIT -- Cool "
                               "starts at None (the museum write above only touched the "
                               "SEPARATE SaveBlock1 painting record, not this mon's own "
                               "ribbons word)")

    s.tap("A", settle=BIG_SETTLE)          # EDIT mode, fsel=0 (Cool) -> em_field_press: 0<->Master
    s.shot("08_ribbons_master", "#60: A on the Cool row toggles None<->Master (em_field_press, "
                                 "pdna_edit.c) -- the per-mon ribbon rank editor, independent "
                                 "of the museum painting")

    s.tap("B", settle=BIG_SETTLE)          # EDIT -> VIEW (keeps the pending edit, dirty=true)
    s.tap("B", settle=BIG_SETTLE)          # leaving with a dirty edit -> "Save changes?" confirm
    s.tap("A", settle=BIG_SETTLE)          # A = write (verified-write pipeline, same as everywhere)
    return s


def run_b218_gender_glyph(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #218: the SUMMARY's POKEMON INFO card's Spec./OT rows used to call
    ui_text() directly, which runs libtonc's tte_write -> real UTF-8 decode ->
    tte_putc's gid = ch - charOffset with NO bound check against the 96-glyph sys8
    font. pk_species_name() encodes NIDORAN's gender sign as the 3-byte UTF-8
    sequence E2 99 80/82 (data_tables.c:14), so that row hit an out-of-bounds glyph
    read on every Nidoran (F) or (M) SUMMARY -- proven on a delta frame BEFORE this
    fix: "Spec. NIDORAN" followed by a garbage glyph (not blank, not '?').

    The fix moved the Spec. and OT rows onto ui_ptext_fit -- the SAME bounded font
    (source/ui.c's pnext()) the Name row two lines below already used, so this
    shot is parity, not new behaviour: the gender sign now renders as one safe '?'
    glyph on every row, no garbage cell anywhere on the card.

    Nav: box screen -> R x7 (lands on the 20/30-occupied box the corpus save
    happens to have at that position) -> DOWN x3, RIGHT x2 (row 3 col 2 -- the
    first EMPTY cell after the box's 20 occupied slots, verified by a probe shot
    before this script existed) -> A (EMPTY/CREATE/PASTE/CANCEL menu) -> A
    (CREATE -> pick_species(1), sel=0 Bulbasaur) -> the picker is the real-art
    7-wide ICON GRID on this build (mon_icon_for(1) != 0), so DOWN moves by
    GCOLS=7 per press, not by list rows -- DOWN x4 (0 -> 7 -> 14 -> 21 -> 28)
    lands exactly on sel=28 = NIDORAN (F), national dex #29, the lowest-dex
    entry with a gender sign in its name -- then A picks it, builds the mon
    (gen3_build_mon_spread), and opens the SIX-CARD SUMMARY on card 0 directly
    (pdna_inspect_create)."""
    s = Session(core_mod, image_mod, rom, out_dir, "b218_")
    print("== BACKLOG #218: SUMMARY Spec./OT rows vs a 3-byte gender sign ==")

    s.press_n("R", 7, settle=BIG_SETTLE)   # box screen -> the box with 10 empty cells
    s.press_n("DOWN", 3)                   # row 0 -> row 3
    s.press_n("RIGHT", 2)                  # col 0 -> col 2 (first empty cell, index 20)
    s.tap("A", settle=BIG_SETTLE)          # empty cell -> EMPTY/CREATE/PASTE HERE/CANCEL
    s.shot("00_empty_cell_menu", "#218 setup: an empty box cell's menu -- CREATE is the "
                                  "second row, already selected")

    s.tap("A", settle=BIG_SETTLE)          # CREATE -> pick_species(1); real-art build -> icon grid
    s.press_n("DOWN", 4)                   # GCOLS=7: 0 -> 7 -> 14 -> 21 -> 28 = NIDORAN (F), #029
    s.shot("01_picker_nidoran", "#218 setup: the species picker cursor on NIDORAN (F), "
                                 "national #029 -- the header already shows the gender sign "
                                 "PokeDNA's own font renders fine at THIS size (type-badge "
                                 "row), unrelated to the SUMMARY card bug below")
                                 # no claim= here: this frame is the real-art icon grid (many
                                 # colours), not the GB-shell font gb_claims.py's matcher reads

    s.tap("A", settle=BIG_SETTLE)          # confirm species -> builds the mon, opens the SUMMARY
    s.shot("02_summary_fixed", "#218 FIXED: SUMMARY, POKEMON INFO card -- Spec. and Name "
                                "both render \"NIDORAN?\" (one safe '?' glyph for the 3-byte "
                                "gender sign, ui_ptext_fit's pnext() bound), no garbage cell "
                                "anywhere on the card; BEFORE this fix the Spec. row showed "
                                "\"NIDORAN\" plus an out-of-bounds glyph instead",
           claim=["NIDORAN?"])

    s.tap("B", settle=BIG_SETTLE)          # leave the summary without keeping the created mon
    s.shot("03_discard_confirm", "#218: leaving the summary without pressing START -- the "
                                  "usual discard confirm, so the box stays untouched")
    return s


def run_b107_contest_picker(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #107: the Contests donor picker on pdna_pick.c's shared pick_rows()
    engine -- source list, a box's mon list with the icon column, a live search
    (osk_search, typed through the real OSK grid) and a sort toggle (START).

    Nav: box screen -> START -> DOWN x15 (Contests, same PDNA_NAV_ITEMS order
    run_contests() uses) -> A (museum, sel=0 Cool) -> A (donor picker, source
    list) -> DOWN (Party(0) -> Box 1(1)) -> A (mon list, box 1).

    The search types a single lowercase 'a' through the real on-screen keyboard
    (source/osk.c's KB grid: row 0 col 0 is '1', so DOWN x2 lands on row 2 ("asdfghjkl"),
    col 0 = 'a') -- ci_contains() is case-insensitive (pdna_pick.c's up1()), so this
    matches any species/nickname containing an "a", virtually guaranteed in a full
    box; the point is proving the search widget is wired to pick_rows, not exercising
    every match (that is tests/host_*'s job). Exits via B all the way out (no A on a
    donor, no museum write -- this script only proves the picker's OWN chrome, unlike
    run_contests() above which also exercises the write path)."""
    s = Session(core_mod, image_mod, rom, out_dir, "b107_")
    print("== BACKLOG #107: donor picker gets pick_rows' chrome ==")

    s.tap("START", settle=BIG_SETTLE)      # box screen -> nav menu
    s.press_n("DOWN", 15)                  # Party(0) .. Fly(14) -> Contests(15)
    s.tap("A", settle=BIG_SETTLE)          # -> pdna_contest, museum page, sel=0 (Cool)
    s.tap("A", settle=BIG_SETTLE)          # A on Cool -> donor picker, source list (pick_rows, PR_ROWH9)
    s.shot("01_source", "#107: donor picker, source list -- Party then the 14 PC boxes, "
                         "now on pick_rows' plain FILT geometry (source_row)")

    s.tap("DOWN", settle=BIG_SETTLE)       # Party(sel=0) -> Box 1(sel=1)
    s.tap("A", settle=BIG_SETTLE)          # A on Box 1 -> mon list (pick_rows, PR_ROWH26 if real art)
    s.shot("02_box_icons", "#107: donor picker, mon list -- Box 1's occupied slots, "
                            "species + level, 24x24 icon column (mon_row, "
                            "mon_icon_for_form_frame) when real art is linked")

    s.tap("SEL", settle=BIG_SETTLE)     # -> osk_search("SEARCH", ...)
    s.press_n("DOWN", 2)                   # row 0 ('1234567890') -> row 2 ('asdfghjkl'), col 0
    s.tap("A", settle=SETTLE)              # insert 'a' (osk_core: KEY_A inserts KB[cr][cc])
    s.tap("START", settle=BIG_SETTLE)      # confirm -> osk_search returns "a"; the list re-filters
    s.shot("03_search", "#107: donor picker, mon list filtered by the live OSK search "
                         "(typed 'a' -- ci_contains is case-insensitive, matches any "
                         "species/nickname containing an A)")

    s.tap("START", settle=BIG_SETTLE)      # pick_rows: START -> sort toggle (No. -> A-Z)
    s.shot("04_sort", "#107: the same filtered list, START-sorted A-Z by species name "
                       "(mon_key doubles as pick_rows' search key and sort key)")

    s.tap("B", settle=BIG_SETTLE)          # mon list -> source list (no pick made)
    s.tap("B", settle=BIG_SETTLE)          # source list -> museum page (no pick made)
    s.tap("B", settle=BIG_SETTLE)          # leave Contests -> box screen (no write at all)
    return s


def run_b107_contest_picker_artless(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #107, the artless half: the SAME box mon list with no icon capability
    linked (mon_icon_for(1) == 0) -- mon_row's plain-FILT fallback (PR_ROWH9, no icon
    slot), not a placeholder graphic. Run against an ARTLESS-flavoured fused build."""
    s = Session(core_mod, image_mod, rom, out_dir, "b107a_")
    print("== BACKLOG #107 (artless): donor picker mon list, no icon column ==")

    s.tap("START", settle=BIG_SETTLE)
    s.press_n("DOWN", 15)
    s.tap("A", settle=BIG_SETTLE)
    s.tap("A", settle=BIG_SETTLE)          # museum -> donor picker source list
    s.tap("DOWN", settle=BIG_SETTLE)       # Party -> Box 1
    s.tap("A", settle=BIG_SETTLE)          # -> mon list, artless fallback (PR_ROWH9)
    s.shot("05_artless_fallback", "#107: the artless build's donor picker mon list -- "
                                   "mon_icon_for(1) == 0, so mon_row falls back to the "
                                   "same plain FILT row source_row uses, no icon slot "
                                   "(pick_species' own `lst` idiom, not a placeholder)")

    s.tap("B", settle=BIG_SETTLE)
    s.tap("B", settle=BIG_SETTLE)
    s.tap("B", settle=BIG_SETTLE)
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emerald", type=Path, required=True,
                     help="pokedna-delta.gba fused with an ordinary Gen-3 .sav "
                          "(tools/fuse_sav.py, no --gb) — boots straight to the box screen")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb",
                     help="SAME dir tools/gb_shots.py writes to, so the shared "
                          "manifest.json / tools/gb_contact_sheet.py --per-feature "
                          "(default --shots) picks these up with no extra flags")
    ap.add_argument("--only",
                     choices=("osk", "flags", "contests", "daycare",
                              "b107-contest", "b107-contest-artless", "b218-gender-glyph"),
                     default=None,
                     help="run just ONE of this script's shot functions (BACKLOG #114's "
                          "pixel-parity proof uses --only daycare against a private --out "
                          "so the before/after cmp never touches the shared manifest.json; "
                          "BACKLOG #107's b107-contest/-artless need TWO different fused "
                          "builds -- real art vs artless -- so run each separately)")
    a = ap.parse_args(argv)

    if not a.emerald.is_file():
        sys.exit(f"--emerald: {a.emerald}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba()

    fn_by_name = {"osk": run_osk_rename, "flags": run_flags_sections,
                  "contests": run_contests, "daycare": run_daycare,
                  "b107-contest": run_b107_contest_picker,
                  "b107-contest-artless": run_b107_contest_picker_artless,
                  "b218-gender-glyph": run_b218_gender_glyph}
    fns = (fn_by_name[a.only],) if a.only else (run_osk_rename, run_flags_sections, run_contests)

    ok, skipped = [], []
    for fn in fns:
        sess = fn(core_mod, image_mod, a.emerald, a.out)
        ok += sess.taken
        skipped += sess.skipped

    # Same merge-not-overwrite manifest.json convention as gb_shots.py's own main()
    # (keyed by filename, so a re-run of just this script only touches its own
    # b2osk_*/b2flags_* entries and never disturbs gold_*/red_*/e4_* ones already
    # written by tools/gb_shots.py against the same --out).
    manifest_path = a.out / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    for n, c, claim_info in ok:
        # BACKLOG #218 fix: Session.taken is a 3-tuple (BACKLOG #184's claim_info,
        # tools/gb_shots.py's own main() already merges it this way at line 778 --
        # this file's main() was never updated when Session grew claim_info, so
        # every run with a claim=/claim_absent= shot crashed here with "too many
        # values to unpack" before writing its manifest entry.
        entry = {"file": n, "caption": c}
        entry.update(claim_info)
        by_file[n] = entry
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")

    print(f"\n{len(ok)} shot(s) saved to {a.out}")
    for name, reason in skipped:
        print(f"[skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
