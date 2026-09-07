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
    a = ap.parse_args(argv)

    if not a.emerald.is_file():
        sys.exit(f"--emerald: {a.emerald}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba()

    ok, skipped = [], []
    for fn in (run_osk_rename, run_flags_sections):
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
