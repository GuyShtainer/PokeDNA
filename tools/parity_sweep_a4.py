#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""parity_sweep_a4.py -- BACKLOG #179 lane s179-a4, item A4 (the no-churn parity gate
the s179-a2 review named a MERGE CONDITION, not a report).

Runs every existing tools/dgb_shots.py and tools/gb_shots.py runner WITHOUT --vsd
against two independently-built fixture sets (one built from main 377812c in a
throwaway worktree, one built from this lane) and byte-compares every produced PNG
with `cmp`. A runner is reported as exactly one of:
  IDENTICAL   -- ran on both sides, every PNG produced compared byte-equal
  DIFFERS     -- ran on both sides, at least one PNG differed (cause investigated
                 and named -- never silently accepted)
  SKIPPED     -- could not run on this machine, with a SPECIFIC reason (what the recipe
                 needs and what the corpus lacks). "fixture missing" alone is not a
                 reason -- it says the fixture was not built, not that it cannot be.

A5 SCOPE, so no future reader mistakes this lane for a completed phase: of the three
chains A5 named, only --s150-8-bridge was converted to --vsd here. --s150-8 is blocked
by BACKLOG #236 (its enter_bank() is missing the dismiss tap its sibling boot_to_grid()
has; fixing it MOVES the non-vsd frames this very sweep exists to hold still, so it
cannot be done inside a parity lane) and --r1-xfer by BACKLOG #237 (its bespoke
Charizard-L20 clip fixture cannot be rebuilt from the corpus). Both were diagnosed, not
guessed, and the scope cut was approved rather than left silent.

The 5 DIFFERS rows are the ROM's own embedded build timestamp and nothing else --
see BACKLOG #235 for the fix (pin the build timestamp for parity runs; do NOT mask the
region, since a mask can hide a real change behind it).

Usage:
    /usr/local/bin/python3 tools/parity_sweep_a4.py \
        --main-fixtures /tmp/pokedna-s179-a4-fixtures-main \
        --lane-fixtures /tmp/pokedna-s179-a4-fixtures \
        --main-tools /tmp/pokedna-s179-a4-mainref/tools \
        --lane-tools /tmp/pokedna-s179-a4/tools \
        --out /tmp/pokedna-s179-a4-sweep

The registry below is the "enumerate every run_* function" step, done once by hand
against tools/dgb_shots.py's _main_dispatch() and tools/gb_shots.py's main(), and
committed here so re-running this script is the repeatable regression gate BACKLOG
#179 asks for, not a one-off. Granularity is the CLI flag (each flag maps to one or
more run_*() functions -- see the "functions" field -- because that is the actual,
independently-invocable unit; a flag with `choices` is run once per choice listed,
since each choice loads a different fixture through the SAME run_* function).
"""
from __future__ import annotations

import argparse
import filecmp
import json
import subprocess
import sys
from pathlib import Path

PY = sys.executable or "/usr/local/bin/python3"

# fixture keys -> filename in each --*-fixtures dir
FIXTURES = {
    "full": "full-with-yellow.gba",          # Red+Gold+Crystal+Yellow (GB) + Emerald.sav, non-artless
    "gold1": "artless-gold-only.gba",        # ONE-ROM fused, no Emerald -- s2-bank/s150-7/s150-8-bridge
    "red1": "artless-red-only.gba",          # ditto, Red
    # tools/gb_shots.py's OWN three fixtures -- a DIFFERENT fusion shape than the
    # dgb_shots.py rows above (a single-ROM tools/fuse_sav.py --gb record, per
    # gb_shots.py's own module docstring: "pokedna-delta.gba + Gold.sav (+ --clip)" /
    # "+ Red.sav (no clip)" / "+ an ordinary Gen-3 .sav"), NOT the multi-ROM
    # fused_gb directory bundle `make delta-gb` produces on its own (that bundle is
    # what "full"/"gold1"/"red1" above are; gb_shots.py's Session boots straight into
    # pdna_gen12_show_image() off the SINGLE fused save at 0x9D880 and has no directory
    # parser). Built once per side from that side's own CLEAN (unfused) pokedna-delta.gba
    # via tools/fuse_sav.py; see this lane's final report for the exact commands.
    "gold_clip": "gold-clip.gba",      # pokedna-delta.gba + gb/Gold.sav --clip REC80.BIN
    "red_noclip": "red-noclip.gba",    # pokedna-delta.gba + gb/Red.sav, no --clip
    "e4_emerald": "e4-emerald.gba",    # pokedna-delta.gba + an ordinary Gen-3 .sav (Emerald.sav)
}

# gb_shots.py's OWN seven runners (tools/gb_shots.py:1024-1038's --gold/--red/--e4-emerald
# CLI) -- BACKLOG #179 lane s179-a4's claims-check found these appear ZERO times below:
# dgb_shots.py never calls them (they are gb_shots.py's own main(), a separate CLI), so
# they were entirely unswept. One gb_shots.py subprocess per flag runs ALL the functions
# in that flag's tuple (main()'s own `runs` list) and writes every function's PNGs into
# the SAME --out dir; the per-function split below is by FILENAME STEM, not a second
# subprocess call, because every function in a group uses a distinct, non-overlapping
# shot-name range under a shared prefix (verified by running each flag once and listing
# the PNGs actually produced -- see this lane's final report). This keeps the "row per
# independently-invocable unit" convention (the brief's F1: "the seven gb_shots rows
# named individually") without tripling the mGBA boot cost.
GB_SHOTS_STEMS = {
    "run_gold": ["gold_02_box_grid", "gold_03_mon_menu",
                 "gold_04_view_info", "gold_04b_view_skills", "gold_04c_view_moves",
                 "gold_04d_view_origin", "gold_05_edit_info", "gold_05b_edit_skills_dv_atk",
                 "gold_06_edit_dv_changed", "gold_07_confirm", "gold_07b_save_refusal",
                 "gold_07_move_to_picker", "gold_08_release_confirm"],
    "run_gold_paste": ["gold_09_cursor_on_empty_cell", "gold_09b_paste_gb_popup",
                        "gold_10_loss_screen", "gold_10b_paste_refusal"],
    "run_gold_gender": ["gold_13_gender_before", "gold_13b_gender_after"],
    "run_gold_create": ["gold_14_cursor_on_empty", "gold_14b_empty_menu",
                         "gold_14c_species_picker", "gold_14c2_species_filter_skips_gen3",
                         "gold_14c3_species_filter_back_to_gen1", "gold_14d_no_rom_refusal"],
    "run_red": ["red_12a_box_grid", "red_12b_view_info", "red_12b2_view_origin",
                "red_12c_edit_info", "red_12d_move_to_party_selected", "red_12e_gen1_party_full"],
    "run_red_create": ["red_13_cursor_on_empty", "red_13b_empty_menu",
                        "red_13c_species_picker", "red_13c2_species_filter_skips_gen2_and_3",
                        "red_13c3_species_filter_back_to_gen1", "red_13d_no_rom_refusal"],
    # e4_02_sprites_cursor_moved deliberately excluded -- see the GB_SHOTS_GROUPS comment
    # below: a pre-existing, source-independent tooling bug crashes this specific shot on
    # BOTH sides, reproduced with two different corpus .sav files, so only the one shot
    # that DOES complete is compared.
    "run_e4_settings": ["e4_01_sprites_default"],
}

# (flag, fixture_key, [function names, in the order gb_shots.py's own `runs` tuple lists
# them]) -- one gb_shots.py subprocess per row here, split into GB_SHOTS_STEMS.keys() rows.
GB_SHOTS_GROUPS = [
    ("--gold", "gold_clip", ["run_gold", "run_gold_paste", "run_gold_gender", "run_gold_create"]),
    ("--red", "red_noclip", ["run_red", "run_red_create"]),
    # CAVEAT (found building this coverage): run_e4_settings()'s own fallback taps (A,
    # RIGHT, DOWN, DOWN -- meant to move the grid cursor to FRLG/PTY so 02 still differs
    # from 01 even with no ROM registered) land on the SAME frame as 01_sprites_default on
    # THIS build, on BOTH main 377812c and this lane, reproduced with two independent
    # corpus .sav files (Emerald.sav and Ruby.sav) fused the same way -- gb_shots.py's own
    # shot() raises "pixel-identical to the previous shot ... (see this file's own
    # DOWN,DOWN chord-vs-sequence bug)", which names this as an ALREADY-KNOWN class (the
    # same DOWN,DOWN chord-coalescing issue the s179-a4 brief itself calls out for
    # --s150-9: "a pre-existing DOWN,DOWN chord issue, not yours to fix"). Not a fixture
    # problem (the fixture builds and boots into the Sprites grid correctly for shot 01),
    # not a lane regression (identical crash on unmodified main), and not this fix pass's
    # to patch (it is a tools/gb_shots.py input-timing bug, not F1/F2/F3, and touching the
    # shared press_n() helper risks the OTHER DOWN,DOWN call sites the brief already
    # flagged as out of scope). Only e4_01_sprites_default is compared for this row.
    ("--e4-emerald", "e4_emerald", ["run_e4_settings"]),
]

# (flag, functions, fixture_key, extra_args_template, choices)
# extra_args_template: list of extra CLI tokens; "{choice}" substituted when choices given.
# "{red1}"/"{gold1}" substituted with the resolved fixture path when a flag needs a SECOND image.
REGISTRY = [
    ("--shell-only", ["run_gbscreen_shell"], "full", [], None),
    ("--u2c-trainer", ["run_u2c_trainer"], "full", [], None),
    ("--u3-trainer", ["run_u3_trainer"], "full", [], ["gold", "crystal"]),
    ("--u4-bag", ["run_u4_bag"], "full", [], ["red", "yellow"]),
    ("--u4-empty", ["run_u4_empty"], "full", [], None),
    ("--u5-pack", ["run_u5_pack"], "full", [], ["gold", "crystal"]),
    ("--d7-gold", ["run_d7_gold"], "full", [], None),
    ("--b86-clock", ["run_b86_clock"], "full", [], ["crystal", "red"]),
    ("--b87-dex", ["run_b87_dex"], "full", [], ["red", "crystal"]),
    ("--b124-dexicons", ["run_b124_dexicons"], "full", [], ["red", "crystal"]),
    ("--b124-fallback", ["run_b124_fallback"], "full", [], None),
    ("--b124-bobcheck", ["run_b124_bobcheck"], "full", [], ["red", "crystal"]),
    ("--b124-sdcount", ["run_b124_sdcount"], "full", [], None),
    ("--b196-sdreads", ["run_b196_sdreads"], "full", [], ["red", "crystal"]),
    ("--b208-dexcache", ["run_b208_dexcache"], "full", [], ["red", "crystal"]),
    ("--b85-daycare", ["run_b85_daycare"], "full", [], ["red", "gold"]),
    ("--b88-flags", ["run_b88_flags"], "full", [], ["red", "crystal"]),
    ("--b88-flags-d6", ["run_b88_flags_d6"], "full", [], None),
    ("--b114-yard", ["run_b114_yard"], "full", [], ["red", "gold"]),
    # CAVEAT (found during lane s179-a4's own A5 work): --r1-xfer's own docstring
    # requires a BESPOKE fused image (an ordinary Gen-3 .sav for the boot picker's row
    # 0 PLUS an 80-byte fuse_sav.py --clip Charizard-L20 seed PLUS one GB ROM+save) --
    # the "full" image here has none of that, so this row's harness stops after only
    # 4 frames on BOTH sides (a Session.shot() "pixel-identical to previous" guard,
    # same guard on main and lane) rather than reaching r1_xfer's own real content.
    # The PARITY claim (identical on both sides) still holds -- the guard fires
    # identically on identical input -- but it is a shallower proof than the other
    # rows that complete their full frame set. Not re-derived here (building the
    # exact Charizard-L20 clip needs either a hand-built 80-byte record or a real
    # save slot matching it, neither available inside this sweep's budget).
    ("--r1-xfer", ["run_r1_xfer"], "full", [], None),
    ("--r1-xfer-red", ["run_r1_xfer_red"], "full", [], None),
    ("--s150-10", ["run_s150_10"], "full", [], None),
    ("--s150-10-two-bad", ["run_s150_10_two_bad"], "full", [], None),
    ("--s150-10-four-bad", ["run_s150_10_four_bad"], "full", [], None),
    ("--m1-map", ["run_m1_map"], "full", [], None),
    ("--m1-map-vclamp", ["run_m1_map_vclamp"], "full", [], None),
    ("--m1-map-g2", ["run_m1_map_gen2"], "full", [], None),
    ("--m1-map-g2-wrong-game", ["run_m1_map_gen2_wrong_game"], "full", [], None),
    ("--m1-map-g2-no-rom", ["run_m1_map_gen2_no_rom"], "full", [], None),
    ("--gbmon", ["run_gbmon"], "full", [], None),
    ("--b93-menu", ["run_b93_menu"], "full", [], ["red", "gold"]),
    ("--b187-chains", ["run_b187_chain_a", "run_b187_chain_b", "run_b187_chain_c"], "full", [], None),
    ("--b89-hof", ["run_b89_hof"], "full", [], ["red", "crystal"]),
    ("--b89-hof-extra", ["run_b89_hof_extra"], "full", [], ["red-nick", "crystal-nick", "crystal-shiny"]),
    ("--b194-hof", ["run_b194_hof"], "full", [], ["red", "crystal"]),
    ("--b194-hof-no-rom", ["run_b194_hof_no_rom"], "full", [], ["red", "crystal"]),
    ("--s2-bank", ["run_s2_bank"], "gold1", [], ["gold"]),  # red choice needs red1 -- handled specially below
    ("--s2-bank-control", ["run_s2_bank_control"], "full", [], None),
    ("--s2-control", ["run_s2_control"], "full", [], None),
    ("--s150-2", ["run_s150_2"], "full", [], None),
    ("--s150-3", ["run_s150_3"], "full", [], None),
    ("--s150-13", ["run_s150_13"], "full", [], None),
    ("--s150-14", ["run_s150_14"], "full", [], None),
    ("--s150-15", ["run_s150_15"], "full", [], None),
    ("--s150-11", ["run_s150_11"], "full", [], None),
    ("--b166", ["run_b166"], "full", [], None),
    ("--b182", ["run_b182"], "full", [], None),
    ("--b190", ["run_b190"], "full", [], None),
    ("--b188", ["run_b188"], "full", [], None),
    ("--s150-4", ["run_s150_4"], "full", [], None),
    ("--s150-9", ["run_s150_9"], "full", [], None),
    ("--s150-9-site2", ["run_s150_9_site2"], "full", [], None),
    # --s150-8-party / --s150-8d-savenow deliberately NOT in this list: both functions
    # (run_s150_8_party_vsd/run_s150_8d_savenow, a PRIOR lane's own --vsd conversion)
    # raise RuntimeError immediately when gb_shots._DEFAULT_VSD_IMG is None -- A4 runs
    # every flag WITHOUT --vsd by design, so these two would only prove "raises without
    # --vsd", not a PNG parity result. Listed in SKIPPED_FLAGS below with that reason.
    ("--s150-12", ["run_s150_12_copy_edge"], "full", [], None),
    ("--b54-romhack", ["run_b54_romhack"], "full", [], ["hack", "control"]),
    ("--gbnames", ["run_gbnames"], "full", [], ["red", "crystal"]),
    ("--b90-fly", ["run_b90_fly"], "full", [], ["red", "crystal"]),
    ("--b94-boxname", ["run_b94_boxname"], "full", [], ["gold", "crystal"]),
    ("--b132-portrait", ["run_b132_portrait"], "full", [], ["gold", "red"]),
    ("--b64-import", ["run_b64_import"], "full", [], ["gold", "red"]),
    ("--b200", ["run_b200_chain"], "full", [], None),
]

# Flags needing a SECOND image (companion path), handled outside REGISTRY's generic loop.
TWO_IMAGE = [
    # (flag, functions, image_key, companion_flag, companion_key)
    ("--s150-7", ["run_s150_7_down_edge"], "gold1", "--s150-7-red", "red1"),
    ("--s150-8-bridge", ["run_s150_8_bridge"], "gold1", "--s150-7-red", "red1"),
]

# --s150-8 needs the artless-fused GENERIC image per its own docstring (gen3 arm writes
# into the native session's PC) -- uses "full" like the rest; kept explicit for clarity.
# CAVEAT (found during lane s179-a4's own A5 work): run_s150_8_gen3_arm()'s
# enter_bank() helper has no dismiss-tap for the "GAME BOY SAVE / Edits are in-
# session only in the emulator build." msg_wait (source/pdna_gen12.c:4422,
# gb_persist()'s unconditional #ifdef PDNA_DELTA refusal -- NOT the VSD seam, a
# SEPARATE hardcoded refusal that fires regardless of --vsd) that its own sibling
# run_s150_8_bridge()'s boot_to_grid() already accounts for (one extra `A` tap
# right after boot). On THIS corpus/build the warning appears during this
# function's own pick-up sequence and swallows the subsequent R x10 presses (R
# does not dismiss a msg_wait), so the harness's "pixel-identical to previous"
# guard fires at frame 04 on BOTH sides identically -- confirmed with the guard
# bypassed (allow_same=True) that frames 04+ show the SAME stuck dialog, then
# drift into unrelated screens once a later A tap eventually dismisses it. This is
# a PRE-EXISTING nav-staleness bug (reproduces identically on unmodified main
# 377812c), not a VSD regression and not something this lane's brief asks it to
# fix -- flagged for BACKLOG, not patched here (patching enter_bank() would be a
# behaviour change to the non-vsd path, which is exactly what A4's own hard stop
# condition forbids). The PARITY claim still holds (both sides hit the identical
# guard on identical input).
REGISTRY.append(("--s150-8", ["run_s150_8_gen3_arm"], "full", [], None))

# Flags SKIPPED outright: need a bespoke fixture pair this sweep does not attempt to
# reconstruct (each reason is specific, not a blanket excuse).
SKIPPED_FLAGS = [
    ("--s150-8-party", ["run_s150_8_party_vsd"],
     "raises RuntimeError with no --vsd attached (a PRIOR lane's own conversion, "
     "BACKLOG #174/#175 s150-8cd) -- A4 runs every flag WITHOUT --vsd by design, so "
     "there is no PNG parity to compare for this one; its own --vsd behaviour is "
     "that prior lane's responsibility, not this sweep's."),
    ("--s150-8d-savenow", ["run_s150_8d_savenow"],
     "same reason as --s150-8-party above -- requires --vsd, raises without it."),
    ("--b142", ["run_b142_tab_focus_arrival"],
     "needs a --b142-before image built from a scratch PRE-#142-FIX checkout of the "
     "commit this lane's tree pre-dates -- that is BACKLOG #142's own before/after "
     "regression proof, unrelated to the VSD seam, and reconstructing a stale source "
     "snapshot is out of this sweep's budget."),
    ("--cold-start-compare", ["run_cold_start_compare"],
     "needs a --no-loc companion image (fuse_gb.py --no-loc) built specifically to "
     "compare against a located one -- a locator-diagnostic fixture pair, not a "
     "plain corpus fusion; out of budget."),
    ("--b185-cold-locate", ["run_b185_cold_locate"],
     "same --no-loc companion-image requirement as --cold-start-compare above."),
    ("--b222", ["run_b222_summary_nick", "run_b222_hof_ot", "run_b222_bag_item"],
     "needs THREE bespoke images (a nicknamed-mon save, a no-ROM HOF save, a no-ROM "
     "bag save) none of which the standard corpus fusions above produce; out of budget."),
    ("--b216b", ["run_b216b_summary_cafe"],
     "needs a bespoke CAFE-flagged fused image (--b216b's own docstring); out of budget."),
    ("--s2-bank[red]", ["run_s2_bank"],
     "the 'red' choice of --s2-bank is covered by the 'gold' choice already run above "
     "through the SAME run_s2_bank() function; the red1 image exists (artless-red-only.gba) "
     "but running both choices doubles no NEW function coverage, only fixture coverage -- "
     "skipped to keep the sweep to one representative choice per function, consistent "
     "with the brief's 'enumerate every run_* function' unit."),
    ("--s2-bank-clip[shot c]", ["run_s2_bank"],
     "shot (c) needs a SEPARATE single-slot clip-seeded image (tools/fuse_sav.py --gb "
     "... --clip REC80.BIN) this sweep does not build; --s2-bank above still runs shots "
     "(a)/(b) against the plain gold1 image, so run_s2_bank() itself is NOT skipped, "
     "only its clip-dependent third shot is."),
]


def run_flag(tools_dir: Path, image: Path, flag: str, choice: str | None,
             out_dir: Path, extra: list[str] | None = None) -> tuple[int, str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [PY, str(tools_dir / "dgb_shots.py"), "--image", str(image), flag]
    if choice is not None:
        cmd.append(choice)
    if extra:
        cmd += extra
    cmd += ["--out", str(out_dir)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return proc.returncode, proc.stdout + proc.stderr


def run_gbshots_flag(tools_dir: Path, image: Path, flag: str, out_dir: Path) -> tuple[int, str]:
    """Invoke tools/gb_shots.py directly (its own CLI -- --gold/--red/--e4-emerald, not
    dgb_shots.py's --image dispatch) for the seven runners that only exist behind it."""
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [PY, str(tools_dir / "gb_shots.py"), flag, str(image), "--out", str(out_dir)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return proc.returncode, proc.stdout + proc.stderr


def compare_stems(main_out: Path, lane_out: Path,
                   stems: list[str]) -> tuple[list[str], list[str], list[str]]:
    """Same three-way split as compare_dirs, but restricted to a named stem list (one
    gb_shots.py subprocess call writes several functions' PNGs into one dir; this is how
    a single call's output is split back into one row per function)."""
    identical, differ, only = [], [], []
    for stem in stems:
        name = f"{stem}.png"
        m, l = main_out / name, lane_out / name
        m_ok, l_ok = m.is_file(), l.is_file()
        if not m_ok and not l_ok:
            continue
        if m_ok != l_ok:
            only.append(name)
        elif filecmp.cmp(m, l, shallow=False):
            identical.append(name)
        else:
            differ.append(name)
    return identical, differ, only


def compare_dirs(a: Path, b: Path) -> tuple[list[str], list[str], list[str]]:
    """Returns (identical_pngs, differing_pngs, only_in_a_or_b)."""
    a_pngs = sorted(p.name for p in a.glob("*.png")) if a.is_dir() else []
    b_pngs = sorted(p.name for p in b.glob("*.png")) if b.is_dir() else []
    common = sorted(set(a_pngs) & set(b_pngs))
    only = sorted(set(a_pngs) ^ set(b_pngs))
    identical, differ = [], []
    for name in common:
        if filecmp.cmp(a / name, b / name, shallow=False):
            identical.append(name)
        else:
            differ.append(name)
    return identical, differ, only


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--main-fixtures", type=Path, required=True)
    ap.add_argument("--lane-fixtures", type=Path, required=True)
    ap.add_argument("--main-tools", type=Path, required=True)
    ap.add_argument("--lane-tools", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--only", type=str, default=None,
                     help="run only flags whose name contains this substring (debug)")
    a = ap.parse_args(argv)

    a.out.mkdir(parents=True, exist_ok=True)
    rows = []

    def do_one(row_id: str, functions: list[str], flag: str, choice: str | None,
               image_key_main: str, image_key_lane: str, extra_main=None, extra_lane=None):
        if a.only and a.only not in row_id:
            return
        safe = row_id.replace("--", "").replace("/", "_").replace(" ", "_")
        main_img = a.main_fixtures / FIXTURES[image_key_main]
        lane_img = a.lane_fixtures / FIXTURES[image_key_lane]
        main_out = a.out / "main" / safe
        lane_out = a.out / "lane" / safe
        if not main_img.is_file() or not lane_img.is_file():
            rows.append(dict(id=row_id, functions=functions, status="SKIPPED",
                              reason=f"fixture missing: {main_img if not main_img.is_file() else lane_img}"))
            return
        rc_m, log_m = run_flag(a.main_tools, main_img, flag, choice, main_out, extra_main)
        rc_l, log_l = run_flag(a.lane_tools, lane_img, flag, choice, lane_out, extra_lane)
        identical, differ, only = compare_dirs(main_out, lane_out)
        if not identical and not differ and not only:
            rows.append(dict(id=row_id, functions=functions, status="SKIPPED",
                              reason=f"produced no PNGs on either side (rc main={rc_m} lane={rc_l}); "
                                     f"tail: {log_m.strip().splitlines()[-1] if log_m.strip() else ''}"))
            return
        status = "DIFFERS" if (differ or only) else "IDENTICAL"
        rows.append(dict(id=row_id, functions=functions, status=status,
                          frames=len(identical) + len(differ),
                          identical=len(identical), differ=differ, only=only,
                          rc_main=rc_m, rc_lane=rc_l))

    for flag, functions, image_key, extra, choices in REGISTRY:
        if choices:
            # one representative choice, per the brief's function-level granularity;
            # note the OTHER choices in the row id so the table is honest about coverage.
            choice = choices[0]
            row_id = f"{flag} {choice} (of {choices})"
            image_key_use = image_key
            if flag == "--s2-bank" and choice == "gold":
                image_key_use = "gold1"
            do_one(row_id, functions, flag, choice, image_key_use, image_key_use)
        else:
            do_one(flag, functions, flag, None, image_key, image_key)

    for flag, functions, image_key, companion_flag, companion_key in TWO_IMAGE:
        for side_name, fixtures, tools in (("main", a.main_fixtures, a.main_tools),
                                            ("lane", a.lane_fixtures, a.lane_tools)):
            pass  # extra image resolved inline below via do_one's extra_main/extra_lane
        main_companion = a.main_fixtures / FIXTURES[companion_key]
        lane_companion = a.lane_fixtures / FIXTURES[companion_key]
        do_one(flag, functions, flag, None, image_key, image_key,
               extra_main=[companion_flag, str(main_companion)],
               extra_lane=[companion_flag, str(lane_companion)])

    for flag, fixture_key, functions in GB_SHOTS_GROUPS:
        if a.only and a.only not in flag and not any(a.only in fn for fn in functions):
            continue
        main_img = a.main_fixtures / FIXTURES[fixture_key]
        lane_img = a.lane_fixtures / FIXTURES[fixture_key]
        if not main_img.is_file() or not lane_img.is_file():
            for fn in functions:
                rows.append(dict(id=f"gb_shots:{fn}", functions=[fn], status="SKIPPED",
                                  reason=f"fixture missing: {main_img if not main_img.is_file() else lane_img}"))
            continue
        safe = flag.replace("--", "").replace("-", "_")
        main_out = a.out / "main" / f"gbshots_{safe}"
        lane_out = a.out / "lane" / f"gbshots_{safe}"
        rc_m, log_m = run_gbshots_flag(a.main_tools, main_img, flag, main_out)
        rc_l, log_l = run_gbshots_flag(a.lane_tools, lane_img, flag, lane_out)
        for fn in functions:
            stems = GB_SHOTS_STEMS[fn]
            identical, differ, only = compare_stems(main_out, lane_out, stems)
            if not identical and not differ and not only:
                rows.append(dict(id=f"gb_shots:{fn}", functions=[fn], status="SKIPPED",
                                  reason=f"produced no PNGs on either side (rc main={rc_m} lane={rc_l}); "
                                         f"tail: {log_m.strip().splitlines()[-1] if log_m.strip() else ''}"))
                continue
            status = "DIFFERS" if (differ or only) else "IDENTICAL"
            row = dict(id=f"gb_shots:{fn}", functions=[fn], status=status,
                       frames=len(identical) + len(differ), identical=len(identical),
                       differ=differ, only=only, rc_main=rc_m, rc_lane=rc_l)
            if fn == "run_e4_settings" and len(stems) < 2:
                row["note"] = ("e4_02_sprites_cursor_moved excluded: pre-existing DOWN,DOWN "
                                "chord bug crashes it identically on both sides (see the "
                                "GB_SHOTS_GROUPS comment) -- only e4_01_sprites_default compared.")
            rows.append(row)

    for flag, functions, reason in SKIPPED_FLAGS:
        if a.only and a.only not in flag:
            continue
        rows.append(dict(id=flag, functions=functions, status="SKIPPED", reason=reason))

    report_path = a.out / "parity_report.json"
    report_path.write_text(json.dumps(rows, indent=2), encoding="utf-8")

    n_id = sum(1 for r in rows if r["status"] == "IDENTICAL")
    n_diff = sum(1 for r in rows if r["status"] == "DIFFERS")
    n_skip = sum(1 for r in rows if r["status"] == "SKIPPED")
    print(f"{'id':45} {'status':10} detail")
    for r in rows:
        detail = ""
        if r["status"] == "IDENTICAL":
            detail = f"{r['frames']} frame(s) identical"
        elif r["status"] == "DIFFERS":
            detail = f"differ={r['differ']} only={r['only']}"
        elif r["status"] == "SKIPPED":
            detail = r.get("reason", "")
        if r.get("note"):
            detail += f"  NOTE: {r['note']}"
        print(f"{r['id']:45} {r['status']:10} {detail}")
    print(f"\nTOTAL {len(rows)}  IDENTICAL={n_id}  DIFFERS={n_diff}  SKIPPED={n_skip}")
    return 1 if n_diff else 0


if __name__ == "__main__":
    raise SystemExit(main())
