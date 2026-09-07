#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gb_shots.py — headless mGBA screenshots of the Gen-1/2 arc (S1..S5-B).

WHY THIS EXISTS
----------------
The Game Boy fork in source/pdna_main.c's view_save() lives entirely inside
`#ifndef PDNA_DELTA` — the emulator build has never been able to reach it, because
that build has no SD card to read a .sav off of. Nobody has ever had a screenshot
of S1..S5-B. docs/HANDOFF.md 2026-09-05 added a narrow, delta-only path: when the
image fused into pokedna-delta.gba (tools/fuse_sav.py --gb) is a Game Boy save
instead of a Gen-3 one, view_save() short-circuits straight into
pdna_gen12_show_image() and loops there. That is the only door this script needs.

WHAT THIS DRIVES
----------------
Two fused images (built by the caller, this script does not fuse anything):
  pokedna-delta.gba + Gold.sav (+ --clip, an 80-byte real Gen-3 box record) — the
    Gen-2 run: info page, box grid, mon menu, the native summary (BACKLOG #41,
    source/pdna_gbsummary.c) in VIEW across all FOUR cards then in EDIT (+ a DV
    edit), the confirm screen, MOVE TO, RELEASE, and — because a clip was seeded —
    PASTE (GB) on an empty cell through to the loss screen.
  pokedna-delta.gba + Red.sav (no clip) — the Gen-1 run: box grid, the native
    summary VIEW (card 0 INFO + card 3 ORIGIN, which shows the record's own G1
    type bytes) + EDIT (no Item/Friendship rows — gbe_fields() drops them outside
    GB_GEN2), and the "Gen 1: withdraw it in-game instead" refusal MOVE TO -> Party
    produces (source/pdna_layout.h PDNA_GBEDIT_MOVE_NEEDSBASE_L2, source/pdna_gen12.c).

BACKLOG #51/#50 (2026-09-07, this session): run_gold_gender() shows the new Gender
row (source/gb_editor.c's GBE_GENDER) flipping Bulbasaur M -> F live, header and
all. run_gold_create()/run_red_create() drive the new CREATE action on an empty
cell (source/pdna_gen12.c's gb_create_hook) through the species picker and level
picker in BOTH generations, to the honest "Needs your Gen 1/2 ROM" wall
gb_new_mon()'s own ROM lookup hits with no flashcart in the emulator — the same
shape 07b_save_refusal/10b_paste_refusal already use for an SD-backed action. The
species/level pickers and the honest refusal ARE captured; the actual build (the
new mon's own summary editor, and it landing in the grid) needs a real ROM on a
real SD card and is hardware-only (docs/HW-TEST-2026-09-05-GB-ARC.md §O).

BACKLOG #41 slice E1 (2026-09-05/06, Guy: "I prefer the summary edit design to be
like we did for gen 3 ... [with] less editable stats"): source/pdna_gbsummary.c
restyled to the SAME Gen-3 CARD chrome pdna_summary.c uses — the shared left info
panel (portrait/name/level/type via the origin-art router), the card dots, and a
MOVING-OUTLINE selection (pdna_summary_sel_frame_*) instead of the earlier retail-
page design's inline per-row highlight box — over FOUR cards (INFO / SKILLS /
MOVES / ORIGIN, the last one new: the honest "this is a Gen 3 preview, not a real
transfer" story + the sidecar link status) instead of the original BACKLOG #41
three-card retail-page layout (INFO / STATS / MOVES, "STATS" renamed "SKILLS").
Shots 04*/05*/06*/07*/12b* below were re-shot for the new chrome and for the new
ORIGIN card; the navigation itself (L/R flips cards, U/D moves the field cursor)
was already Gen-3-parity from BACKLOG #41's first landing and is unchanged here.
BACKLOG #40's four cosmetic nits (box banner capacity, Gen-1 box-name spacing, the
paste-refusal title, the party-full wording) are also visible in several captions.

Every SD-backed action (a card write, i.e. anything past "confirm") refuses in the
emulator — mGBA has no flashcart. That refusal is captured too: it is honest
evidence the never-corrupt gate holds even with nowhere to write, not a bug.

Usage:
    /usr/local/bin/python3 tools/gb_shots.py --gold FUSED_GOLD.gba --red FUSED_RED.gba \
        --out docs/shots/gb

Frame-advance idiom (docs/HANDOFF.md's mGBA harness + the auto-repeat gotcha this
script's own trial run hit): every screen in this app enables d-pad AUTO-REPEAT
(key_repeat_mask), so a press held too long or fired right after a screen
transition can silently advance twice. Every tap here is a short hold + a long
release/settle, and every action that opens or closes a whole screen gets an
extra settle on top before the next tap is sent.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

KEY = dict(A=0x1, B=0x2, SEL=0x4, START=0x8, RIGHT=0x10, LEFT=0x20,
           UP=0x40, DOWN=0x80, R=0x100, L=0x200)

HOLD = 3          # frames a key is physically "down"
SETTLE = 12       # frames of nothing, after a simple cursor move
BIG_SETTLE = 40   # frames of nothing, after a screen opens/closes/repaints fully


def load_mgba():
    vendor = ROOT.parent / "rec2mp4" / "vendor"
    if not vendor.is_dir():
        sys.exit(f"vendor path missing: {vendor} (docs/HANDOFF.md's mGBA harness recipe)")
    sys.path.insert(0, str(vendor))
    import mgba.core, mgba.image, mgba.log   # noqa: E402
    mgba.log.silence()                        # MANDATORY: else per-instruction logs flood stderr
    return mgba.core, mgba.image


class Session:
    """One booted core + the tap/shot primitives every shot list below is built from."""

    def __init__(self, core_mod, image_mod, rom_path: Path, out_dir: Path, prefix: str):
        self.core = core_mod.load_path(str(rom_path))
        if self.core is None:
            sys.exit(f"mgba could not load {rom_path}")
        self.screen = image_mod.Image(*self.core.desired_video_dimensions())
        self.core.set_video_buffer(self.screen)   # BEFORE reset()
        self.core.reset()
        self.out_dir = out_dir
        self.prefix = prefix
        self.taken = []     # (filename, caption)
        self.skipped = []   # (label, reason)
        self._last_shot = None    # (name, raw RGB bytes) -- for the consecutive-differ check
        self.run(180)       # let the boot screen (info page) fully settle

    def run(self, n: int) -> None:
        for _ in range(n):
            self.core.run_frame()

    def tap(self, *names: str, settle: int = SETTLE) -> None:
        """A single key-down edge. Multiple names are pressed as ONE simultaneous chord
        (A+B together), NOT sequential presses -- ORing the same name twice collapses to
        one edge, which is exactly the bug that sent an intended double-DOWN into a
        single row move on the first pass of this script (07/08 opened the wrong menu
        row as a result). Use press_n() for "the same key, N times in a row"."""
        mask = 0
        for n in names:
            mask |= KEY[n]
        self.core.set_keys(raw=mask)
        self.run(HOLD)
        self.core.set_keys(raw=0)
        self.run(settle)

    def press_n(self, name: str, n: int, settle: int = SETTLE) -> None:
        for _ in range(n):
            self.tap(name, settle=settle)

    def shot(self, name: str, caption: str, settle: int = 0) -> Path:
        if settle:
            self.run(settle)
        img = self.screen.to_pil().convert("RGB")
        path = self.out_dir / f"{self.prefix}{name}.png"

        # Fail LOUDLY here rather than write a plausible-but-wrong PNG a human has to
        # catch by eye later (exactly the class of bug the DV-edit and MOVE-TO/RELEASE
        # navigation mistakes earlier in this script's own history were: a tap silently
        # not landing, leaving the emulator on the previous screen).
        lo, hi = img.convert("L").getextrema()
        if lo == hi:
            raise RuntimeError(
                f"{self.prefix}{name}: captured frame is a single flat colour "
                f"(value {lo}) -- the emulator is very unlikely to be showing a real "
                f"screen; a settle/press was probably too short or landed mid-transition.")
        raw = img.tobytes()
        if self._last_shot is not None and raw == self._last_shot[1]:
            raise RuntimeError(
                f"{self.prefix}{name}: pixel-identical to the previous shot "
                f"({self._last_shot[0]}) -- the tap(s) between them had no visible "
                f"effect, so this is very likely the same screen twice, not two "
                f"different ones (see this file's own DOWN,DOWN chord-vs-sequence bug).")
        self._last_shot = (name, raw)

        img.save(path)
        self.taken.append((path.name, caption))
        print(f"  [ok]   {path.name:32s} {caption}")
        return path

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append((name, reason))
        print(f"  [skip] {name:32s} {reason}")


def run_gold(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2, --clip seeded) ==")

    s.shot("01_info", "S1: the info page — Gold/Silver save, converted-copy notice, counts")

    s.tap("A", settle=BIG_SETTLE)          # info -> box grid
    s.shot("02_box_grid", "S2: box grid — GB BOX1 20/20 (BACKLOG #40(a): the banner now uses "
                          "the source's own capacity, not the Gen-3 grid's 30 cells)")

    s.tap("A", settle=BIG_SETTLE)          # A on slot 0 (Bulbasaur) -> mon menu
    s.shot("03_mon_menu", "S2: the read-only mon menu — Gen-3 parity (BACKLOG #42/#43 batch, "
                          "2026-09-05): VIEW and EDIT are now ONE row (\"VIEW / EDIT\", same "
                          "label the Gen-3 menu uses), then MOVE TO/RELEASE/LEGALITY/COPY")

    # ---- MOVE TO: menu order is now VIEW/EDIT, MOVE TO, RELEASE, LEGALITY, COPY, CANCEL
    # (one row shorter than before the VIEW/EDIT merge). ----
    s.press_n("DOWN", 1)                   # VIEW/EDIT -> MOVE TO
    s.tap("A", settle=BIG_SETTLE)          # open the box/party picker
    s.shot("07_move_to_picker", "S3: MOVE TO — the destination box/party picker")
    s.tap("B", settle=BIG_SETTLE)          # cancel — do not actually move anything

    # ---- RELEASE: back at the box grid; A -> menu -> DOWN x2 -> RELEASE ----
    s.tap("A", settle=BIG_SETTLE)
    s.press_n("DOWN", 2)                   # VIEW/EDIT -> MOVE TO -> RELEASE
    s.tap("A", settle=BIG_SETTLE)
    s.shot("08_release_confirm", "S3: RELEASE — the confirm popup")
    s.tap("B", settle=BIG_SETTLE)          # cancel — do not actually release it

    # ---- BACKLOG #41: VIEW now opens pdna_gbsummary.c over the NATIVE record (three
    # cards) instead of pdna_inspect() on the lossy Gen-3-converted copy. A -> VIEW
    # (the menu's first row, already selected). ----
    s.tap("A", settle=BIG_SETTLE)
    s.tap("A", settle=BIG_SETTLE)          # VIEW (already selected) -> the summary, card 0 INFO
    s.shot("04_view_info", "BACKLOG #41: the native summary, VIEW, card 0 INFO — "
                            "nickname/level/type/OT/item/friendship/EXP")
    s.tap("R", settle=BIG_SETTLE)          # card 0 -> 1
    s.shot("04b_view_skills", "BACKLOG #41 slice E1: VIEW, card 1 SKILLS — HP/Atk/Def/Spe/"
                               "SpA/SpD, each a two-row cell (value, then DV + stat exp)")
    s.tap("R", settle=BIG_SETTLE)          # card 1 -> 2
    s.shot("04c_view_moves", "BACKLOG #41: VIEW, card 2 MOVES — 4 moves, PP cur/max, PP Ups")
    s.tap("R", settle=BIG_SETTLE)          # card 2 -> 3
    s.shot("04d_view_origin", "BACKLOG #41 slice E1: VIEW, card 3 ORIGIN (new) — the honest "
                               "\"Gen 3 preview only / edits change the GB save\" story, plus "
                               "the sidecar link status")
    s.tap("L", settle=BIG_SETTLE)          # card 3 -> back to 2, so B below leaves from MOVES
                                            # (matches the pre-E1 script's own exit point)
    s.tap("B", settle=BIG_SETTLE)          # leave (not dirty -> no confirm) -> box grid

    # ---- EDIT: Gen-3 parity (BACKLOG #42/#43 batch) retired the standalone EDIT row that
    # opened straight into edit mode -- the merged VIEW/EDIT row always opens in VIEW, and
    # (like the Gen-3 summary) A INSIDE it flips to edit mode (pdna_gbsummary.c's own KEY_A
    # handler, c->can_edit). menu -> VIEW/EDIT (already selected) -> A again for edit. ----
    s.tap("A", settle=BIG_SETTLE)
    s.tap("A", settle=BIG_SETTLE)          # VIEW/EDIT (already selected) -> the summary, VIEW
    s.tap("A", settle=BIG_SETTLE)          # A inside VIEW -> editing = true, same as Gen 3
    s.shot("05_edit_info", "Gen-3 parity: A inside the VIEW/EDIT summary flips to edit "
                            "mode, card 0 — the frame on Nickname (fsel resets on entry)")

    s.tap("R", settle=BIG_SETTLE)          # card 0 -> 1 (SKILLS); fsel resets to 0 (HP's SE)
    s.tap("DOWN", settle=SETTLE)           # fsel 0 (SE0/HP) -> 1 (DV Atk) — card_skills()'s
                                            # own registration order, stat_row() called
                                            # HP/ATK/DEF/SPE/SpA/SpD in that order and each
                                            # registers DV-then-SE, so index 1 is ATK's DV.
    s.shot("05b_edit_skills_dv_atk", "BACKLOG #41 slice E1: EDIT, card 1 SKILLS, the moving "
                                      "outline (not an inline highlight any more) on DV Atk")
    # This Bulbasaur's DV Atk starts at 15 (max). pdna_gbsummary reserves L/R for CARD
    # FLIP (Gen-3 pdna_inspect's own convention — BACKLOG #41 asked for "the same feel
    # as gen 3"), so there is no big-step shoulder adjust here any more (SELECT still
    # reaches pdna_gbedit.c's flat list, which keeps it) — plain LEFT taps, one per DV
    # point, the same [0,15] clamp gb_editor.c's dv_stat enforces. Not every tap lands
    # as a discrete -1 once mGBA's own key-repeat kicks in over this many presses (a
    # measured run stopped at DV 4, not 0) — the DEMONSTRATION this shot exists for is
    # the live header flip, which needs "low enough", not exactly zero.
    for _ in range(15):
        s.tap("LEFT", settle=6)
    s.shot("06_edit_dv_changed", "BACKLOG #41: DV Atk drops low enough to flip the header "
                                  "M -> F live (Gen-2 gender-from-DV), same as the old flat editor")

    s.tap("B", settle=BIG_SETTLE)          # edit mode -> VIEW (edits kept, gbedit_confirm not shown yet)
    s.tap("B", settle=BIG_SETTLE)          # leave the mon; dirty -> gbedit_confirm() (shared with pdna_gbedit.c)
    s.shot("07_confirm", "BACKLOG #41: leaving with unsaved edits — the SAME gbedit_confirm() "
                          "panel pdna_gbedit.c uses (shared helper, not a second copy)")
    s.tap("A", settle=BIG_SETTLE)          # A = write -> gb_edit_commit -> gb_persist refuses (no SD in mGBA)
    s.shot("07b_save_refusal", "BACKLOG #41: confirmed -> gb_persist refuses (no SD card in "
                                "mGBA) — honest evidence, not a bug")
    s.tap("B", settle=BIG_SETTLE)          # dismiss / back out however far this landed
    s.tap("B", settle=BIG_SETTLE)

    return s


def run_gold_gender(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #51: the Gender row, a fresh boot independent of run_gold()'s own in-place
    DV edit above (same reasoning as run_gold_paste's own separate boot). Bulbasaur
    (box slot 0) has a real, non-fixed gender ratio (0x7F, 50/50) — gbe_has_gender_row()
    shows the row for exactly this reason (source/gb_editor.c)."""
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #51 Gender row ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid
    s.tap("A", settle=BIG_SETTLE)          # slot 0 (Bulbasaur) -> mon menu
    s.tap("A", settle=BIG_SETTLE)          # VIEW/EDIT -> the summary, VIEW, card 0 INFO
    s.tap("A", settle=BIG_SETTLE)          # A inside VIEW -> editing = true
    s.press_n("DOWN", 2)                   # Name -> Lv -> Gender (card_info's own row order,
                                            # source/pdna_gbsummary.c: NICK, LEVEL, GENDER, OT...)
    s.shot("13_gender_before", "#51: the Gender row, field cursor on it — Bulbasaur reads "
                                "M (Atk DV 15 under this save's own ratio 0x7F)")
    s.tap("A", settle=BIG_SETTLE)          # gbe_flip_gender: nearest Atk DV of the other gender
    s.shot("13b_gender_after", "#51: A flips it — Gender now reads F, and the shared left "
                                "panel's header (name/level/gender chip) updates live in the "
                                "SAME repaint, same as the old DV-edit demo (06_edit_dv_changed) "
                                "used to show indirectly")
    s.tap("B", settle=BIG_SETTLE)          # edit -> view (edits kept, not yet asked to write)
    s.tap("B", settle=BIG_SETTLE)          # leave; dirty -> gbedit_confirm (not shot here, see 07_confirm)
    s.tap("B", settle=BIG_SETTLE)          # discard, whichever confirm/summary layer this landed on
    return s


def run_gold_create(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #50: CREATE on an empty cell. GB BOX1 (the box the info page always opens
    into) is 20/20 on this corpus, same as every box run_gold() already notes — box index
    13 ("GB BOX13" on screen) has real room, 17/20, discovered the same way tools/
    gb_retail_gate.py's first_room_box() does. This is ALSO the run that caught the
    box-resolution bug fixed before this file existed in its current form: an EARLIER
    version of gb_create_hook trusted app_mon_menu's own (box, slot) parameters, which
    pdna_box.c hard-codes to (0, cur) for every is_bank source regardless of which box is
    actually on screen — CREATE from box 13 kept refusing "BOX FULL" against box 0's
    real 20/20, caught by hand on exactly this navigation before the fix (Gb12Mount.ui_box)
    landed. Species/level pickers need no SD card (pure UI, no file I/O); gb_new_mon's own
    ROM lookup does, so it is the honest stopping point in an emulator with no flashcart —
    the SAME "SD-backed action refuses in the emulator, captured as evidence" shape
    07b_save_refusal/10b_paste_refusal already use."""
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #50 CREATE ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid (GB BOX1, 20/20)
    s.press_n("R", 13, settle=SETTLE)      # -> GB BOX13, 17/20 (real room)
    s.press_n("DOWN", 2)
    s.press_n("RIGHT", 5)                  # cursor -> slot 17, the first real empty slot
    s.shot("14_cursor_on_empty", "#50: cursor on a real empty slot (GB BOX13, 17/20)")

    s.tap("A", settle=BIG_SETTLE)          # open the empty-cell menu
    s.shot("14b_empty_menu", "#50: the empty-cell menu now offers CREATE (was PASTE (GB) + "
                              "CANCEL only, before this backlog item) — CREATE first, "
                              "matching Gen-3's own empty-cell menu order")

    s.tap("A", settle=BIG_SETTLE)          # select CREATE -> the species picker
    s.shot("14c_species_picker", "#50: the species picker — dex-style rows (\"No.NNN NAME\"), "
                                  "names only, no icon grid; a SEPARATE picker from pdna_pick.c's "
                                  "big pick_species() (see gb_create_pick_species's own comment)")

    s.tap("A", settle=BIG_SETTLE)          # pick Bulbasaur (already selected) -> the level picker
    s.shot("14d_level_picker", "#50: the level picker — default level 5, LEFT/RIGHT +-1, "
                                "L/R +-10 (gbe_adjust's own GBE_LEVEL step sizes)")

    s.tap("RIGHT", settle=SETTLE)
    s.tap("RIGHT", settle=SETTLE)
    s.shot("14e_level_7", "#50: LEFT/RIGHT adjusts the level — 5 -> 7 after two taps")
    s.tap("LEFT", settle=SETTLE)
    s.tap("LEFT", settle=SETTLE)           # back to 5, so the ROM lookup below builds Lv5

    s.tap("A", settle=BIG_SETTLE)          # confirm the level -> gb_create_locate_rom
    s.shot("14f_no_rom_refusal", "#50: confirmed -> \"Needs your Gen 1/2 ROM\" — honest "
                                  "evidence, not a bug: mGBA has no flashcart/SD card, so "
                                  "neither the registered-ROM path (app_gb_rom_path) nor "
                                  "the beside-the-save fallback can ever find one here")
    s.skip("14g_new_mon_summary",
           "gb_new_mon()'s own ROM lookup (rom_gbbase_gen1/2 + rom_gblearn) needs a real "
           "Gen-1/2 ROM on the SD card, which mGBA's fused-save harness has no flashcart "
           "to provide (same class of gap as E4's Sprites-era-cycling shot) -- the summary "
           "editor this backlog item opens over the newly built mon is real, host-tested "
           "code (tests/host_newmon_test.c, gb_new_mon()) but only reachable with a real "
           "cartridge. Hardware-only: docs/HW-TEST-2026-09-05-GB-ARC.md §O.")
    s.skip("14h_mon_in_grid",
           "downstream of 14g (gbs_insert -> the box grid re-paging with the new mon "
           "showing) -- same ROM-needs-a-real-SD-card gap, hardware-only.")
    return s


def run_gold_paste(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """Separate fresh boot for the PASTE (GB) shots: independent of run_gold()'s in-place
    DV edit above, so a failure there can never take these down with it."""
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2, --clip) -- PASTE (GB) on an empty cell ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid

    # Grid is 6 cols x 5 rows; slots 0..19 are filled, 20..29 are the "always empty"
    # cells S5-B's own review notes call out. Slot 20 = row 3, col 2 from slot 0.
    s.press_n("DOWN", 3)
    s.press_n("RIGHT", 2)
    s.shot("09_cursor_on_empty_cell", "S5-B: cursor parked on an empty cell before pressing A")

    s.tap("A", settle=BIG_SETTLE)
    s.shot("09b_paste_gb_popup", "S5-B: an empty cell's action menu — just PASTE (GB) + CANCEL")

    s.tap("A", settle=BIG_SETTLE)          # select PASTE (GB) -> gen3_to_gb() -> the loss screen
    s.shot("10_loss_screen", "S5-B: the Gen-3-to-GB loss screen (what a real transfer would drop)")

    # This corpus's GB BOX1 is already at its real Gen-2 capacity (20/20 -- the "20/30"
    # badge counts 10 structurally-dead display cells too, source/pdna_gen12.c's own
    # note on cells 20..29). gb_session.c's gbs_move()/gbs_insert() check capacity
    # BEFORE anything else, including before the card is ever opened for writing, so
    # this is the real, first refusal a paste into this box hits -- not the SD-write
    # refusal the header below once assumed. It is still honest evidence: the capacity
    # gate holds even for a converted paste. gold_06b already covers the SD-refusal
    # path (a plain edit-and-save, which has no capacity gate in the way).
    s.tap("A", settle=BIG_SETTLE)
    s.shot("10b_paste_refusal", "S5-B: confirmed -> refused: GB BOX1 is already full (20/20), "
                                 "the same capacity gate a real transfer would hit")
    return s


def run_red(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "red_")
    print("== Red.sav (Gen 1, no clip) ==")

    s.tap("A", settle=BIG_SETTLE)
    s.shot("12a_box_grid", "S2: Red.sav box grid — Gen 1")

    # BACKLOG #41: VIEW opens the native summary (card 0 INFO — the requested Gen-1 shot).
    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.tap("A", settle=BIG_SETTLE)          # VIEW (already selected)
    s.shot("12b_view_info", "BACKLOG #41: Gen-1 native summary, VIEW, card 0 INFO — no "
                             "Item/Friendship rows (Gen-1 has neither)")
    s.press_n("R", 3, settle=BIG_SETTLE)   # INFO -> SKILLS -> MOVES -> ORIGIN
    s.shot("12b2_view_origin", "BACKLOG #41 slice E1: Gen-1, card 3 ORIGIN — the Type row "
                                "reads the record's OWN stored type bytes (g1_to_g3_type()), "
                                "not the species table the shared left panel's chips use")
    s.press_n("L", 3, settle=BIG_SETTLE)   # back to INFO, so the B below leaves from the
                                            # same card the pre-E1 script always did
    s.tap("B", settle=BIG_SETTLE)          # leave -> box grid

    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.tap("A", settle=BIG_SETTLE)          # VIEW/EDIT (already selected) -> the summary, VIEW
    s.tap("A", settle=BIG_SETTLE)          # A inside VIEW -> editing = true, same as Gen 3
    s.shot("12c_edit_info", "Gen-3 parity: A inside the VIEW/EDIT summary flips to edit "
                             "mode — no Item/Friendship rows (gbe_fields() drops them "
                             "outside GB_GEN2)")
    s.tap("B", settle=BIG_SETTLE)          # edit -> view
    s.tap("B", settle=BIG_SETTLE)          # back to box grid (no edits made -> no confirm)

    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.press_n("DOWN", 1)                   # VIEW/EDIT -> MOVE TO
    s.tap("A", settle=BIG_SETTLE)
    # Party is the LAST row in the box/party picker for a Gen-1 source (gb_edit.c
    # GEN1_PARTY_BOX has the highest box number, so it sorts last). The picker's cursor
    # wraps (sel == 0 ? n-1 : sel-1 on UP, matching every other list in this app), so one
    # UP from the default top-of-list selection always lands on the last entry -- no
    # need to guess how many storage boxes this save has.
    s.tap("UP")
    s.shot("12d_move_to_party_selected", "S2b: MOVE TO picker, Party selected")
    s.tap("A", settle=BIG_SETTLE)
    # gb_session.c gbs_move() checks capacity (dcount >= gb_list_capacity) BEFORE the
    # Gen-1 box->party GBS_ERR_NEEDS_BASE check ("Gen 1: withdraw it in-game instead",
    # source/pdna_layout.h PDNA_GBEDIT_MOVE_NEEDSBASE_L2) ever runs. Guy's own Red.sav
    # and Yellow.sav (gba-toolkit/roms/gb/) both carry a full 6/6 party, so this specific
    # refusal is unreachable from the corpus on hand -- it always hits GBS_ERR_FULL
    # first. That IS the real, correct behaviour (still worth a shot), not a script bug.
    s.shot("12e_gen1_party_full", "S2b: Gen 1 MOVE TO -> Party refused: the party is "
                                   "already full (6/6) — the capacity gate that runs "
                                   "before the Gen-1-specific one. BACKLOG #40(d): the "
                                   "message now reads \"The party is full.\", not the "
                                   "box-flavoured \"that box is full\"")
    s.skip("12f_gen1_needs_base_text",
           "the \"Gen 1: withdraw it in-game instead\" text (GBS_ERR_NEEDS_BASE) is only "
           "reached when the destination party has a free slot; both of Guy's Gen-1 "
           "saves (Red.sav, Yellow.sav) have a full 6/6 party, so gbs_move()'s own "
           "capacity check (gb_session.c:399, checked first) wins every time. Needs a "
           "save with room in the party to ever show this exact string.")

    s.skip("11_dv_orphan_warning", "needs a sidecar file on SD (docs/GEN3-TO-GB-SIDECAR-DESIGN.md "
                                    "section 10) — the emulator has no SD card, so has_sidecar is "
                                    "always false and the warning never fires. Real-hardware only.")
    return s


def run_red_create(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #50: CREATE on an empty cell, Gen 1 — the SAME flow run_gold_create() drives,
    on the generation that has no Gender row (BACKLOG #51 is Gen-2-only, gbe_has_gender_row
    refuses outright for GB_GEN1) and whose species picker is capped at dex 151, not 251
    (gb_max_species(GB_GEN1)). "GB BOX5" (R press count 4 from the box grid's own default
    entry) is 19/20 on this corpus — one real empty slot."""
    s = Session(core_mod, image_mod, rom, out_dir, "red_")
    print("== Red.sav (Gen 1) -- BACKLOG #50 CREATE ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid
    s.press_n("R", 4, settle=SETTLE)       # -> GB BOX5, 19/20 (one real empty slot)
    s.press_n("DOWN", 3)
    s.press_n("RIGHT", 1)                  # cursor -> slot 19, the one real empty slot
    s.shot("13_cursor_on_empty", "#50: Gen 1 — cursor on the one real empty slot (GB BOX5, 19/20)")

    s.tap("A", settle=BIG_SETTLE)
    s.shot("13b_empty_menu", "#50: Gen 1's empty-cell menu also offers CREATE — identical "
                              "row list to Gen 2's (gb_create_hook does not special-case "
                              "the generation for the menu itself, only for the ROM lookup "
                              "and the moveset merge behind it)")

    s.tap("A", settle=BIG_SETTLE)          # select CREATE -> the species picker
    s.shot("13c_species_picker", "#50: Gen 1's species picker — dex 1..151 only "
                                  "(gb_max_species(GB_GEN1)), same names-only rows as Gen 2's")

    s.tap("A", settle=BIG_SETTLE)          # pick Bulbasaur -> the level picker
    s.shot("13d_level_picker", "#50: the level picker — identical control for both "
                                "generations (gb_create_pick_level takes no gen argument)")

    s.tap("A", settle=BIG_SETTLE)          # confirm level 5 -> gb_create_locate_rom
    s.shot("13e_no_rom_refusal", "#50: Gen 1 hits the same honest \"Needs your Gen 1/2 ROM\" "
                                  "wall as Gen 2 — mGBA has no flashcart for either generation's "
                                  "ROM lookup to find one on")
    s.skip("13f_new_mon_summary",
           "same gap as Gold's 14g: gb_new_mon()'s ROM lookup needs a real cartridge. "
           "Hardware-only: docs/HW-TEST-2026-09-05-GB-ARC.md §O.")
    return s


def run_e4_settings(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """E4 (docs/SPRITE-ERA-DESIGN.md): Settings > Game ROM > the Sprites era grid.
    `rom` is pokedna-delta.gba fused with an ordinary GEN-3 .sav (tools/fuse_sav.py) --
    NOT a Game Boy image; this is the one path in this script that opens a normal
    Gen-3 box screen and drives the nav menu, not the GB fork the rest of the file is
    about. Reachable only as of commit b5788db ("Sprites grid reachable in the
    emulator build"): before that, PDNA_DELTA's Settings > Game ROM row was a flat
    "NO SD HERE" refusal with no live sub-screen at all.

    Navigation (session boots straight to the box screen; no info page here, unlike
    the GB fork's pdna_gen12_show_image): START -> nav menu (two 10-row columns,
    RIGHT jumps a whole column, sel 0 Party -> RIGHT -> sel 10 Blocks) -> DOWN x7 to
    NV_SETTINGS (index 17: 10 + 7) -> A -> the Settings list (S_BACKUP..S_CLOSE) ->
    DOWN x3 to S_ROM -> A -> (PDNA_DELTA only) straight into sprite_settings(), no
    "Change ROM" popup in the way.
    """
    s = Session(core_mod, image_mod, rom, out_dir, "e4_")
    print("== E4: Settings > Sprites grid (fused Gen-3 save, no GB fork) ==")

    s.tap("START", settle=BIG_SETTLE)      # box screen -> nav menu
    s.tap("RIGHT")                          # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", 7)                    # Blocks -> ... -> Settings (index 17)
    s.tap("A", settle=BIG_SETTLE)           # nav menu -> Settings list
    s.press_n("DOWN", 3)                    # Backups -> Animations -> Yard -> Game ROM
    s.tap("A", settle=BIG_SETTLE)           # S_ROM -> (delta) straight into the grid

    s.shot("01_sprites_default", "E4: Settings > Game ROM > Sprites — default state, "
           "every cell NATIVE; the GEN1/GEN2 rows show \"-\" under PC (the two dead "
           "cells se_cell_applies() defines: a Game Boy kind has no Gen-3-style PC box)")

    # No ROM is registered in this fused-save session (there is no second file for the
    # emulator to have registered), so se_era_next() can only ever offer NATIVE back --
    # cycling the selected cell with A is a real keypress but a visible no-op. Per the
    # coordinator's own fallback: also move the cursor so the second shot still differs
    # from the first, and say so plainly rather than imply a cycle that did not happen.
    s.tap("A", settle=SETTLE)               # attempt to cycle RS/PC -> stays "Native"
    s.tap("RIGHT", settle=SETTLE)           # PC -> PTY
    s.press_n("DOWN", 2, settle=SETTLE)     # RS -> EM -> FRLG

    s.shot("02_sprites_cursor_moved",
           "E4: A on a cell with no ROM registered leaves it NATIVE (se_era_next has "
           "nothing else to offer) — cursor moved to FRLG/PTY so this shot still "
           "differs from the default; cycling to a real era needs a registered ROM, "
           "which the fused-save emulator session has no second file to provide "
           "(hardware-only, docs/HW-TEST-2026-09-05-GB-ARC.md §K)")

    s.tap("B", settle=BIG_SETTLE)           # save (cfg_save no-ops: no SD) -> Settings list
    s.tap("B", settle=BIG_SETTLE)           # Close/B -> back to nav menu or box grid
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, help="pokedna-delta.gba fused with Gold.sav (+ --clip)")
    ap.add_argument("--red", type=Path, help="pokedna-delta.gba fused with Red.sav")
    ap.add_argument("--e4-emerald", type=Path,
                     help="pokedna-delta.gba fused with an ordinary Gen-3 .sav "
                          "(tools/fuse_sav.py) -- E4: Settings > Sprites grid, NOT a "
                          "Game Boy image")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    a = ap.parse_args(argv)

    runs = [("gold", a.gold, (run_gold, run_gold_paste, run_gold_gender, run_gold_create)),
            ("red", a.red, (run_red, run_red_create)),
            ("e4-emerald", a.e4_emerald, (run_e4_settings,))]
    active = [(label, p, fns) for label, p, fns in runs if p is not None]
    if not active:
        sys.exit("nothing to do: pass at least one of --gold/--red/--e4-emerald")
    for label, p, _fns in active:
        if not p.is_file():
            sys.exit(f"--{label}: {p}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba()

    ok, skipped = [], []
    for _label, p, fns in active:
        for fn in fns:
            sess = fn(core_mod, image_mod, p, a.out)
            ok += sess.taken; skipped += sess.skipped

    # A manifest, not just a list printed to stdout: tools/gb_contact_sheet.py reads this
    # so a shot's caption lives in exactly one place (this file) instead of being
    # retyped wherever the sheet gets built.
    #
    # MERGED with whatever is already there, not overwritten: a run given only
    # --e4-emerald (say) must not erase the S1..S5-B entries a separate --gold/--red
    # run already wrote — this file is the union of every partial run against the
    # same --out, keyed by filename/name so re-running a given shot updates its own
    # caption in place instead of duplicating it.
    import json
    manifest_path = a.out / "manifest.json"
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

    print(f"\n{len(ok)} shot(s) saved to {a.out}")
    for name, reason in skipped:
        print(f"[skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
