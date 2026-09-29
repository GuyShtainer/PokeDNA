#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_grid_blocked_test.py -- structural guard for lane b200's three fixes
(BACKLOG #200): the Gen-1/2 grid's phantom cells. A GB box holds 20 mons (party
6) but the grid always draws 30 cells; before this lane nothing marked slots
20-29, so a full Yellow box's bottom two rows looked empty, the cursor could
land there, and CREATE said BOX FULL -- this is what Guy read as "the slot is
not empty, it's not occupied" (BACKLOG #187). Same posture as
tests/host_gb_grid_ops_test.py (b187): pure-text checks over the shipped
source, no mgba, no build.

Three checks:

  (a) F1: blocked_cells() exists (source/pdna_box.c), loops from `cap` to
      COLS*ROWS painting the blocked tile, and is wired at every wallpaper
      repaint site the BG cell layers already share (render_full, move_cursor,
      chunk_draw -- the same three era_cells()/artless_cells() call sites).

  (b) F2: the cursor is never left resting on a blocked cell. Checked at every
      site the design names: the plain grid's LEFT/RIGHT/DOWN handlers and the
      MOVE-mode carry's LEFT/RIGHT/DOWN handlers route through the capacity-
      aware helpers (grid_lr_step for LEFT/RIGHT, a `cap`-relative bound for
      DOWN, not the old fixed COLS*(ROWS-1) literal); the SWITCH_BOX macro
      clamps `cur` to the new box's own last real slot; the b188 resume-cell
      hint is bounds-checked against `cap`, not the raw COLS*ROWS.

  (c) F3: the A handler on a blocked cell (`cur >= cap`) shows the
      PDNA_BOX_NO_SLOT dialog INSTEAD of falling into the EMPTY/CREATE/CANCEL
      menu -- checked as the refusal block appearing before app_mon_menu('s
      call inside the same `else if (k & KEY_A)` arm.

Run directly:

    python3 tests/host_gb_grid_blocked_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #200).
"""
import re
import sys
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_BOX = ROOT / "source" / "pdna_box.c"
SRC_LAYOUT = ROOT / "source" / "pdna_layout.h"


def strip_comments(text: str) -> str:
    """Blank out /* ... */ and // comments but keep every newline, so any line
    number reported still points at the real source line -- same helper
    host_gb_grid_ops_test.py's own copy documents (a prose mention of the
    right token in a COMMENT must never satisfy a check)."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    """Full body (signature through matching close-brace) of the top-level
    function `func_name`, or "" if not found. Brace-depth walk, not a
    fixed-line slice, so it survives the function growing/shrinking. Copied
    verbatim from host_gb_grid_ops_test.py's own helper (b187) -- same idiom,
    intentionally not shared via import (each host test stays a single,
    self-contained file, the existing convention in this tests/ directory)."""
    m = re.search(r"^\w[\w \*]*\b" + re.escape(func_name) + r"\s*\([^;]*?\)\s*\{",
                  text, re.MULTILINE)
    if not m:
        m = re.search(r"\b" + re.escape(func_name) + r"\s*\([^;{]*\)\s*\{", text)
        if not m:
            return ""
    start = m.start()
    i = m.end() - 1
    assert text[i] == "{"
    depth = 1
    j = i + 1
    while j < len(text) and depth > 0:
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
        j += 1
    return text[start:j]


def check_render_marks_blocked(box_text: str) -> list[str]:
    """(a) F1: blocked_cells() loops from `cap` up to COLS*ROWS (the tail of
    blocked cells), and is called at every wallpaper-repaint site
    artless_cells() is (render_full, move_cursor, chunk_draw) -- the codebase's
    own "every site that repaints the wallpaper owes both [now three] BG cell
    layers" invariant, extended to this lane's new layer."""
    code = strip_comments(box_text)
    violations = []
    body = extract_function_body(box_text, "blocked_cells")
    if not body:
        return ["pdna_box.c: blocked_cells() function body not found -- F1's "
                "BLOCKED-cell paint is gone (BACKLOG #200 regression)"]
    body_nc = strip_comments(body)
    if not re.search(r"for\s*\(\s*int\s+i\s*=\s*cap\s*;\s*i\s*<\s*COLS\s*\*\s*ROWS", body_nc):
        violations.append("pdna_box.c: blocked_cells() no longer loops `for (int i = "
                           "cap; i < COLS * ROWS; ...)` -- the blocked-tail paint is "
                           "gone or no longer capacity-driven (BACKLOG #200 regression)")
    # BACKLOG #268: move_cursor no longer repaints the wallpaper (only the banner band via
    # wp_restore_rect), so it owes no cell layer -- and MUST NOT repaint it again, or every
    # Game Boy cell's art is refetched on UP/DOWN to the name row.
    mc = strip_comments(extract_function_body(box_text, "move_cursor"))
    if not mc:
        violations.append("pdna_box.c: move_cursor() function body not found")
    elif "draw_wallpaper(" in mc or "wp_restore_rect(" not in mc:
        violations.append("pdna_box.c: move_cursor() repaints the whole wallpaper (or no "
                          "longer restores just the banner band) -- BACKLOG #268 regression: "
                          "the name-row round trip reloads every Game Boy cell")
    render_sites = ["render_full", "chunk_draw"]
    for fn in render_sites:
        fn_body = strip_comments(extract_function_body(box_text, fn))
        if not fn_body:
            violations.append(f"pdna_box.c: {fn}() function body not found")
            continue
        if "blocked_cells(" not in fn_body:
            violations.append(f"pdna_box.c: {fn}() no longer calls blocked_cells( -- "
                               "a wallpaper repaint from this site would leave blocked "
                               "cells unmarked again (BACKLOG #200 F1 regression)")
    if "for (int i = cap; i < COLS * ROWS; i++)" not in code and not violations:
        pass  # exact-spacing variant already covered by the regex check above
    return violations


def check_cursor_skips_blocked(box_text: str) -> list[str]:
    """(b) F2: every LEFT/RIGHT/DOWN cursor handler the design names is
    capacity-aware, and the two entry points that can place the cursor
    (SWITCH_BOX, the b188 resume-cell hint) clamp/bound against `cap`, not the
    raw COLS*ROWS the grid merely DRAWS."""
    code = strip_comments(box_text)
    violations = []

    # grid_lr_step exists and both plain-grid and carry-mode LEFT/RIGHT route
    # through it (the codebase's shared LEFT/RIGHT skip-blocked-cells helper).
    if "static int grid_lr_step(int cur, int cap, bool right)" not in code:
        violations.append("pdna_box.c: grid_lr_step() helper missing -- F2's LEFT/"
                           "RIGHT skip-over-blocked-cells logic is gone (BACKLOG "
                           "#200 regression)")
    lr_calls = len(re.findall(r"cur\s*=\s*grid_lr_step\s*\(\s*cur\s*,\s*cap\s*,", code))
    if lr_calls < 4:  # 2 call sites (plain grid, MOVE-mode carry) x LEFT+RIGHT each
        violations.append(f"pdna_box.c: only {lr_calls} grid_lr_step(cur, cap, ...) "
                           "call site(s) found, expected at least 4 (LEFT+RIGHT in "
                           "both the plain grid and the MOVE-mode carry handlers) -- "
                           "a cursor handler regressed to the old unconditional wrap "
                           "(BACKLOG #200 F2 regression)")

    # DOWN: both sites must test against `cap`, not the old fixed COLS*(ROWS-1).
    down_cap_tests = len(re.findall(r"cur\s*\+\s*COLS\s*(?:>=|<)\s*cap\b", code))
    if down_cap_tests < 2:  # plain-grid wrap test + carry-mode step test
        violations.append(f"pdna_box.c: only {down_cap_tests} `cur + COLS >=/< cap` "
                           "DOWN test(s) found, expected at least 2 -- DOWN can once "
                           "again walk the cursor onto a blocked cell (BACKLOG #200 F2 "
                           "regression)")
    # The is_bank EXIT (return 5) fires only at the PHYSICAL bottom row (b200 review A1):
    # a capacity edge mid-grid wraps (plain grid) or stays put (carry), never re-enters.
    exit_tests = len(re.findall(r"src->is_bank\s*&&\s*cur\s*\+\s*COLS\s*>=\s*COLS\s*\*\s*ROWS", code))
    if exit_tests < 2:
        violations.append(f"pdna_box.c: only {exit_tests} `src->is_bank && cur + COLS >= COLS * ROWS` "
                           "exit test(s) found, expected 2 -- a capacity edge would exit/re-enter the "
                           "box instead of wrapping (b200 review A1 regression)")

    # SWITCH_BOX clamps cur to the new box's own last real slot.
    switch_body = code[code.find("#define SWITCH_BOX"):code.find("#define SWITCH_BOX") + 900]
    if "cap = box_cap(src, box)" not in switch_body or "cur >= cap" not in switch_body:
        violations.append("pdna_box.c: SWITCH_BOX no longer recomputes `cap` and "
                           "clamps `cur >= cap` on a box switch -- a box switch can "
                           "land the cursor on a blocked cell again (BACKLOG #200 "
                           "F2 regression)")

    # b188 resume-cell hint bounds-checked against capacity, not COLS*ROWS.
    if not re.search(r"rc\s*>=\s*0\s*&&\s*rc\s*<\s*cap\b", code):
        violations.append("pdna_box.c: the resume-cell hint no longer checks `rc < "
                           "cap` -- a resume onto a box that shrank (or a stale "
                           "cell from a different source) can land on a blocked "
                           "cell again (BACKLOG #188/#200 regression)")

    return violations


def check_a_refuses_blocked(box_text: str) -> list[str]:
    """(c) F3: A on a blocked cell shows PDNA_BOX_NO_SLOT instead of opening the
    ordinary EMPTY/CREATE/CANCEL menu -- checked as the refusal appearing INSIDE
    the plain grid's `else if (k & KEY_A)` arm, textually BEFORE its
    app_mon_menu( call (same arm, so the refusal must short-circuit first)."""
    body = strip_comments(extract_function_body(box_text, "pdna_box"))
    if not body:
        return ["pdna_box.c: pdna_box() function body not found"]
    m = re.search(r"else\s+if\s*\(\s*k\s*&\s*KEY_A\s*\)\s*\{"
                  r"[\s\S]{0,700}?cur\s*>=\s*cap[\s\S]{0,400}?PDNA_BOX_NO_SLOT"
                  r"[\s\S]{0,1100}?app_mon_menu\s*\(", body)
    if not m:
        return ["pdna_box.c: the plain grid's KEY_A handler no longer refuses a "
                "blocked cell (`cur >= cap`) with PDNA_BOX_NO_SLOT before reaching "
                "app_mon_menu( -- A on a blocked cell would open the ordinary "
                "EMPTY/CREATE/CANCEL menu again (BACKLOG #200 F3 regression)"]
    return []


def check_no_slot_string_defined(layout_text: str) -> list[str]:
    """F3's dialog string exists in pdna_layout.h (the project's convention --
    strings live there, not as bare literals -- STANDING-RULES/the brief)."""
    if "#define PDNA_BOX_NO_SLOT" not in layout_text:
        return ["pdna_layout.h: PDNA_BOX_NO_SLOT is no longer defined -- F3's "
                "blocked-cell dialog has no string to show (BACKLOG #200 "
                "regression)"]
    return []


def run_all(box_text: str, layout_text: str) -> list[str]:
    return (check_render_marks_blocked(box_text)
            + check_cursor_skips_blocked(box_text)
            + check_a_refuses_blocked(box_text)
            + check_no_slot_string_defined(layout_text))


def main() -> int:
    if not SRC_BOX.exists() or not SRC_LAYOUT.exists():
        print("SKIP (source/pdna_box.c or source/pdna_layout.h not found)")
        return 0

    box_text = SRC_BOX.read_text()
    layout_text = SRC_LAYOUT.read_text()

    violations = run_all(box_text, layout_text)
    if violations:
        print("FAIL -- shipped source has a b200 regression:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: shipped source -- F1's blocked-cell paint (wired at all three "
          "wallpaper-repaint sites), F2's capacity-aware LEFT/RIGHT/DOWN cursor "
          "handling (plain grid + MOVE carry), SWITCH_BOX's clamp, the resume-cell "
          "hint's capacity bound, and F3's PDNA_BOX_NO_SLOT refusal are all present")

    tmpdir = Path(tempfile.mkdtemp(prefix="gbgridblocked_"))
    try:
        box_scratch = tmpdir / "pdna_box.c"
        layout_scratch = tmpdir / "pdna_layout.h"

        # --- mutant 1 (F1): drop the render check -------------------------------
        # Un-wire blocked_cells() from render_full (the site every visible repaint
        # of the box screen goes through first) -- the mutant still PAINTS blocked
        # cells when explicitly asked (blocked_cells() itself is untouched), it
        # just never gets called on the primary paint path.
        target1 = "  blocked_cells(src, box);      /* BACKLOG #200 F1: mark cells past this source's capacity */\n  draw_box_banner(src, box, on_title);\n  draw_footer(src->is_bank, on_title, moving);"
        mutated_line1 = "  draw_box_banner(src, box, on_title);\n  draw_footer(src->is_bank, on_title, moving);"
        if target1 not in box_text:
            print(f"FAIL -- mutant 1 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target1!r}")
            return 1
        box_scratch.write_text(box_text.replace(target1, mutated_line1, 1))
        layout_scratch.write_text(layout_text)
        m1 = check_render_marks_blocked(box_scratch.read_text())
        if not m1:
            print("FAIL -- mutant 1 (drop the render_full() call to blocked_cells) "
                  "did NOT turn check (a) red (vacuous check)")
            return 1
        print("mutant 1 (drop the render call): correctly caught:")
        for v in m1:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- mutant 2 (F2): drop one cursor clamp -------------------------------
        # Revert SWITCH_BOX's own clamp back to the pre-lane shape (recompute
        # `cap` stays -- only the `cur >= cap` clamp line is dropped), so a box
        # switch can land the cursor on a blocked cell again.
        target2 = ("cap = box_cap(src, box); \\\n"
                   "                                 if (cur < 0 || cur >= COLS * ROWS) cur = 0; \\\n"
                   "                                 /* BACKLOG #200 F2: a box switch that would land the \\\n"
                   "                                  * cursor on a blocked cell (a smaller-capacity box, \\\n"
                   "                                  * or the GB party pseudo-box) clamps to the last \\\n"
                   "                                  * real slot instead of resting past it. */ \\\n"
                   "                                 if (cur >= cap) cur = cap - 1; \\\n")
        mutated_line2 = ("cap = box_cap(src, box); \\\n"
                         "                                 if (cur < 0 || cur >= COLS * ROWS) cur = 0; \\\n")
        if target2 not in box_text:
            print(f"FAIL -- mutant 2 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target2!r}")
            return 1
        box_scratch.write_text(box_text.replace(target2, mutated_line2, 1))
        m2 = check_cursor_skips_blocked(box_scratch.read_text())
        if not m2:
            print("FAIL -- mutant 2 (drop SWITCH_BOX's cap clamp) did NOT turn "
                  "check (b) red (vacuous check)")
            return 1
        print("mutant 2 (drop SWITCH_BOX's cap clamp): correctly caught:")
        for v in m2:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- mutant 3 (F3): drop the A refusal ----------------------------------
        target3 = ("      if (cur >= cap) {\n"
                   "        snd_deny();\n"
                   "        ui_clear();\n"
                   "        ui_panel(20, 60, 200, 44, UI_PANEL, UI_WARN);\n"
                   "        ui_ptext_fit(26, 70, 188, UI_WARN, PDNA_BOX_NO_SLOT);\n"
                   "        ui_text(30, 86, UI_DIM, \"Press A\");\n"
                   "        u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);\n"
                   "        need_full = true;\n"
                   "        continue;\n"
                   "      }\n")
        if target3 not in box_text:
            print(f"FAIL -- mutant 3 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target3!r}")
            return 1
        box_scratch.write_text(box_text.replace(target3, "", 1))
        m3 = check_a_refuses_blocked(box_scratch.read_text())
        if not m3:
            print("FAIL -- mutant 3 (drop the A refusal) did NOT turn check (c) "
                  "red (vacuous check)")
            return 1
        print("mutant 3 (drop the A refusal): correctly caught:")
        for v in m3:
            print(f"  (mutated-copy) FAIL: {v}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nhost_gb_grid_blocked_test: ok (shipped source clean, all three "
          "mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
