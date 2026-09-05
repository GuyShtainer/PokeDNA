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
    Gen-2 run: info page, box grid, mon menu, editor (+ a DV edit), the confirm
    screen, MOVE TO, RELEASE, and — because a clip was seeded — PASTE (GB) on an
    empty cell through to the loss screen.
  pokedna-delta.gba + Red.sav (no clip) — the Gen-1 run: box grid, editor (no
    Item/Friendship rows — gbe_fields() drops them outside GB_GEN2), and the
    "Gen 1: withdraw it in-game instead" refusal MOVE TO -> Party produces
    (source/pdna_layout.h PDNA_GBEDIT_MOVE_NEEDSBASE_L2, source/pdna_gen12.c:1075).

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
    s.shot("02_box_grid", "S2: box grid — GB BOX1 20/30 (cells 20..29 are structurally dead)")

    s.tap("A", settle=BIG_SETTLE)          # A on slot 0 (Bulbasaur) -> mon menu
    s.shot("03_mon_menu", "S2: the read-only mon menu — VIEW/EDIT/MOVE TO/RELEASE/LEGALITY/COPY")

    # ---- MOVE TO: menu order is VIEW, EDIT, MOVE TO, RELEASE, LEGALITY, COPY, CANCEL ----
    s.press_n("DOWN", 2)                   # VIEW -> EDIT -> MOVE TO
    s.tap("A", settle=BIG_SETTLE)          # open the box/party picker
    s.shot("07_move_to_picker", "S3: MOVE TO — the destination box/party picker")
    s.tap("B", settle=BIG_SETTLE)          # cancel — do not actually move anything

    # ---- RELEASE: back at the box grid; A -> menu -> DOWN x3 -> RELEASE ----
    s.tap("A", settle=BIG_SETTLE)
    s.press_n("DOWN", 3)                   # VIEW -> EDIT -> MOVE TO -> RELEASE
    s.tap("A", settle=BIG_SETTLE)
    s.shot("08_release_confirm", "S3: RELEASE — the confirm popup")
    s.tap("B", settle=BIG_SETTLE)          # cancel — do not actually release it

    # ---- EDIT: A -> menu -> DOWN x1 -> EDIT ----
    s.tap("A", settle=BIG_SETTLE)
    s.tap("DOWN")                          # VIEW -> EDIT
    s.tap("A", settle=BIG_SETTLE)
    s.shot("04_editor", "S2b: the editor — Nickname/OT/moves/PP, Gen 2 (Item + Friendship rows)")

    # gb_editor.c LABEL[] order for GB_GEN2 (Item/Friendship included):
    # 0 Nickname 1 OT Name 2 OT ID 3 Level 4 Item 5 Friendship 6-9 Move1-4
    # 10-13 MaxPP1-4 14-17 PP1-4 18 DV Atk (19 Def, 20 Spe, 21 Spc) 22 DV HP 23-27 StatExp
    for _ in range(18):
        s.tap("DOWN", settle=6)
    s.shot("04b_editor_at_dv_atk", "S2b: cursor on DV Atk, before the edit (15 -> male)")
    # This Bulbasaur's DV Atk starts at 15 (max) -- +1 has nowhere to go, so decrease with
    # L (big step, -5 per press, verified against gb_editor.c's dv_stat clamp [0,15]).
    # 15 -> 10 -> 5 -> 0 empirically flips the Gen-2 gender formula's result for this mon
    # (checked directly: DV Atk 0 renders "BULBASAUR Lv5 F", not M).
    s.tap("L", settle=BIG_SETTLE)
    s.tap("L", settle=BIG_SETTLE)
    s.tap("L", settle=BIG_SETTLE)
    s.shot("05_editor_dv_changed", "S2b: DV Atk 15 -> 0 — header flips M -> F (Gen-2 gender-from-DV)")

    s.tap("START", settle=BIG_SETTLE)
    s.shot("06_confirm", "S2b: START -> the save-confirm screen")
    s.tap("A", settle=BIG_SETTLE)          # attempt to persist -> no SD in the emulator
    s.shot("06b_save_refusal", "S2b: confirmed -> gb_persist refuses (no SD card in mGBA) — "
                                "honest evidence, not a bug")
    s.tap("B", settle=BIG_SETTLE)          # back out however far this landed
    s.tap("B", settle=BIG_SETTLE)

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

    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.tap("DOWN")                          # VIEW -> EDIT
    s.tap("A", settle=BIG_SETTLE)
    s.shot("12b_editor_gen1", "S2b: Gen-1 editor — no Item/Friendship rows "
                              "(gbe_fields() drops them outside GB_GEN2)")
    s.tap("B", settle=BIG_SETTLE)          # back to box grid

    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.press_n("DOWN", 2)                   # VIEW -> EDIT -> MOVE TO
    s.tap("A", settle=BIG_SETTLE)
    # Party is the LAST row in the box/party picker for a Gen-1 source (gb_edit.c
    # GEN1_PARTY_BOX has the highest box number, so it sorts last). The picker's cursor
    # wraps (sel == 0 ? n-1 : sel-1 on UP, matching every other list in this app), so one
    # UP from the default top-of-list selection always lands on the last entry -- no
    # need to guess how many storage boxes this save has.
    s.tap("UP")
    s.shot("12c_move_to_party_selected", "S2b: MOVE TO picker, Party selected")
    s.tap("A", settle=BIG_SETTLE)
    # gb_session.c gbs_move() checks capacity (dcount >= gb_list_capacity) BEFORE the
    # Gen-1 box->party GBS_ERR_NEEDS_BASE check ("Gen 1: withdraw it in-game instead",
    # source/pdna_layout.h PDNA_GBEDIT_MOVE_NEEDSBASE_L2) ever runs. Guy's own Red.sav
    # and Yellow.sav (gba-toolkit/roms/gb/) both carry a full 6/6 party, so this specific
    # refusal is unreachable from the corpus on hand -- it always hits GBS_ERR_FULL
    # first. That IS the real, correct behaviour (still worth a shot), not a script bug.
    s.shot("12d_gen1_party_full", "S2b: Gen 1 MOVE TO -> Party refused: the party is "
                                   "already full (6/6) — the capacity gate that runs "
                                   "before the Gen-1-specific one")
    s.skip("12e_gen1_needs_base_text",
           "the \"Gen 1: withdraw it in-game instead\" text (GBS_ERR_NEEDS_BASE) is only "
           "reached when the destination party has a free slot; both of Guy's Gen-1 "
           "saves (Red.sav, Yellow.sav) have a full 6/6 party, so gbs_move()'s own "
           "capacity check (gb_session.c:399, checked first) wins every time. Needs a "
           "save with room in the party to ever show this exact string.")

    s.skip("11_dv_orphan_warning", "needs a sidecar file on SD (docs/GEN3-TO-GB-SIDECAR-DESIGN.md "
                                    "section 10) — the emulator has no SD card, so has_sidecar is "
                                    "always false and the warning never fires. Real-hardware only.")
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", required=True, type=Path, help="pokedna-delta.gba fused with Gold.sav (+ --clip)")
    ap.add_argument("--red", required=True, type=Path, help="pokedna-delta.gba fused with Red.sav")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    a = ap.parse_args(argv)

    for p in (a.gold, a.red):
        if not p.is_file():
            sys.exit(f"{p}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba()

    ok, skipped = [], []
    s1 = run_gold(core_mod, image_mod, a.gold, a.out)
    ok += s1.taken; skipped += s1.skipped
    s2 = run_gold_paste(core_mod, image_mod, a.gold, a.out)
    ok += s2.taken; skipped += s2.skipped
    s3 = run_red(core_mod, image_mod, a.red, a.out)
    ok += s3.taken; skipped += s3.skipped

    # A manifest, not just a list printed to stdout: tools/gb_contact_sheet.py reads this
    # so a shot's caption lives in exactly one place (this file) instead of being
    # retyped wherever the sheet gets built.
    import json
    (a.out / "manifest.json").write_text(
        json.dumps({"shots": [{"file": n, "caption": c} for n, c in ok],
                    "skipped": [{"name": n, "reason": r} for n, r in skipped]}, indent=2),
        encoding="utf-8")

    print(f"\n{len(ok)} shot(s) saved to {a.out}")
    for name, reason in skipped:
        print(f"[skip] {name}: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
