#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_grid_ops_test.py -- structural guard for lane b187's four fixes
(BACKLOG #187/#193/#191a/#192): pure-text checks over the shipped source, no
mgba, no build -- same posture tests/host_gb_write_gate_test.py uses for its
own write-safety gate.

Four checks:

  (a) F1's gate shape (source/pdna_box.c, the SELECT dispatch): leaving a
      non-NORMAL mode must be unconditional (`s_cur_mode != CM_NORMAL ||`)
      ahead of the entry capability check -- #192 was exactly this clause
      missing (SELECT on an empty cell in MOVE mode fell to `else snd_deny()`
      forever, proven live with a pixel-identical before/after mGBA capture,
      see this lane's own report).

  (b) F2 is wired both ends: k_gb_xfer's own `.move_within` field is non-NULL
      (source/pdna_gen12.c), AND drop_held's GB same-scope branch
      (source/pdna_box.c) actually calls `src->xfer->move_within(` rather than
      the old `snd_deny(); return recs;` stub -- either half regressing alone
      (the table entry NULLed, or the call site reverted) breaks the feature.

  (c) F3: gb_dup_hook's full-box path (source/pdna_gen12.c) reaches
      `gb_pick_box(` after its own `GBS_ERR_FULL` check -- DUPLICATE must
      offer a destination instead of a flat refusal.

  (d) F3/F4: gb_pick_box's own row-render loop paints a "NAME  n/cap" label
      (a `%d/%d`-shaped format string), not a bare box name -- the picker's
      whole point (both DUPLICATE's and CREATE's new full-box path trust it
      to show room, not just names) is silently lost if this regresses.

  (e) review fix, MEDIUM: gb_move_within_hook's own body (source/pdna_gen12.c)
      calls gb_move_core( -- checks (a)-(d) alone pin that the hook is WIRED,
      not that it actually MOVES anything; a mutant that ignores its
      arguments and returns true unconditionally left the whole suite green
      before this check existed.

  (f) review fix 4, BACKLOG #191a: source/pdna_bank.c's meta_save() -- the
      ONLY path pdna_bank_next_serial() reaches to persist a new serial --
      creates /PokeDNA/bank (f_mkdir, same return-ignored idiom
      pdna_bank_show() already uses) before its own sf_write_verified() call.
      Without this, the first-ever grab on a card whose Bank screen was never
      opened refuses with a silent beep (meta_save's write fails at the
      FatFs layer -- no directory to write into) -- Guy's own #191a report,
      traced by the review to here.

Run directly:

    python3 tests/host_gb_grid_ops_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #187).
"""
import re
import sys
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_BOX = ROOT / "source" / "pdna_box.c"
SRC_GEN12 = ROOT / "source" / "pdna_gen12.c"
SRC_BANK = ROOT / "source" / "pdna_bank.c"


def strip_comments(text: str) -> str:
    """Blank out /* ... */ and // comments but keep every newline, so any line
    number reported still points at the real source line -- same helper
    host_gb_write_gate_test.py's own copy documents (a prose mention of the
    right token in a COMMENT must never satisfy a check)."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    """Full body (signature through matching close-brace) of the top-level
    function `func_name`, or "" if not found. Brace-depth walk, not a
    fixed-line slice, so it survives the function growing/shrinking."""
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


def check_select_gate(box_text: str) -> list[str]:
    """(a) F1's gate shape: leaving a non-NORMAL mode must be unconditional.
    Looked for as ONE else-if condition: `s_cur_mode != CM_NORMAL ||` followed
    (within the same statement, comments stripped) by the entry-capability
    check -- either the new can_enter_move dispatch or, loosely, whatever
    replaces it, matched generously (src_can_lift/can_enter_move) so a future
    rename of the entry-side check does not make this brittle for no reason;
    the load-bearing fact this pins is the UNCONDITIONAL LEAVE clause, not the
    entry check's exact spelling."""
    code = strip_comments(box_text)
    m = re.search(r"else\s+if\s*\(\s*s_cur_mode\s*!=\s*CM_NORMAL\s*\|\|"
                  r"[\s\S]{0,300}?(?:can_enter_move|src_can_lift)\s*\(", code)
    if not m:
        return ["pdna_box.c: no `else if (s_cur_mode != CM_NORMAL || ...)` SELECT "
                "gate found -- leaving a non-NORMAL mode is no longer unconditional "
                "(BACKLOG #192 regression: SELECT can stick again on an empty cell)"]
    return []


def check_move_within_wired(box_text: str, gen12_text: str) -> list[str]:
    """(b) F2 wired both ends."""
    violations = []
    gen12_code = strip_comments(gen12_text)
    m = re.search(r"\bk_gb_xfer\s*=\s*\{(.*?)\};", gen12_code, re.DOTALL)
    if not m or re.search(r"\.move_within\s*=\s*0\s*,", m.group(1)) or \
       not re.search(r"\.move_within\s*=\s*\w+", m.group(1)):
        violations.append("pdna_gen12.c: k_gb_xfer.move_within is NULL/missing -- "
                           "the GB same-scope drop has no real handler to call "
                           "(BACKLOG #191a regression)")

    body = strip_comments(extract_function_body(box_text, "drop_held"))
    if not body:
        violations.append("pdna_box.c: drop_held() function body not found")
    elif "src->xfer->move_within(" not in body:
        violations.append("pdna_box.c: drop_held()'s GB same-scope branch no longer "
                           "calls src->xfer->move_within( -- the old deny-beep stub is "
                           "back (BACKLOG #191a regression)")
    return violations


def check_dup_reaches_picker(gen12_text: str) -> list[str]:
    """(c) F3: gb_dup_hook's full-box path reaches gb_pick_box(."""
    body = strip_comments(extract_function_body(gen12_text, "gb_dup_hook"))
    if not body:
        return ["pdna_gen12.c: gb_dup_hook() function body not found"]
    m = re.search(r"GBS_ERR_FULL[\s\S]{0,400}?gb_pick_box\s*\(", body)
    if not m:
        return ["pdna_gen12.c: gb_dup_hook()'s GBS_ERR_FULL path does not reach "
                "gb_pick_box( within a reasonable span -- DUPLICATE on a full box "
                "refuses flat again instead of offering a destination (BACKLOG #193 "
                "regression)"]
    return []


def check_picker_paints_ncap(gen12_text: str) -> list[str]:
    """(d) F3/F4: gb_pick_box's row render paints a %d/%d-shaped label."""
    body = strip_comments(extract_function_body(gen12_text, "gb_pick_box"))
    if not body:
        return ["pdna_gen12.c: gb_pick_box() function body not found"]
    if not re.search(r'"[^"]*%d\s*/\s*%d[^"]*"', body):
        return ["pdna_gen12.c: gb_pick_box()'s row render has no \"...%d/%d...\" "
                "format string -- the picker no longer shows a box's own count/"
                "capacity (BACKLOG #187/#193 regression: a full box is offered "
                "with no way to tell it apart from one with room)"]
    return []


def check_move_within_moves(gen12_text: str) -> list[str]:
    """(e) review finding, MEDIUM: checks (a)-(d) above pin that move_within is
    WIRED (a non-NULL function pointer reachable from the right call site), but
    none of them pin that the function actually MOVES anything -- a mutant
    `gb_move_within_hook` that ignores its arguments and returns true unconditionally
    left the whole suite green (caught by review, not by this test, before this
    check existed). gb_move_core( is the one primitive that actually calls
    gbs_move()/gb_persist() (see gb_move_hook's own identical call just above it in
    source) -- gb_move_within_hook's body must reach it, not just return a bare
    `true`."""
    body = strip_comments(extract_function_body(gen12_text, "gb_move_within_hook"))
    if not body:
        return ["pdna_gen12.c: gb_move_within_hook() function body not found"]
    if "gb_move_core(" not in body:
        return ["pdna_gen12.c: gb_move_within_hook()'s body no longer calls "
                "gb_move_core( -- it can return success without moving anything "
                "(BACKLOG #191a regression: a same-scope GB drag-and-drop would "
                "silently do nothing while claiming to work)"]
    return []


def check_meta_save_mkdirs(bank_text: str) -> list[str]:
    """(f) review fix 4, BACKLOG #191a: meta_save()'s own body (comments stripped)
    must call f_mkdir( at least twice (the "/PokeDNA" then PDNA_BANK_DIR pair
    pdna_bank_show() already uses) BEFORE its own write call
    (sf_write_verified() -- meta_save's real write; the assignment-only prep code
    above it does not touch the card). Two separate f_mkdir( calls, not one,
    because a straight-to-PDNA_BANK_DIR mkdir fails on a card where "/PokeDNA"
    itself does not exist yet either (same two-level idiom pdna_bank_show() uses)."""
    body = strip_comments(extract_function_body(bank_text, "meta_save"))
    if not body:
        return ["pdna_bank.c: meta_save() function body not found"]
    wm = re.search(r"sf_write_verified\s*\(", body)
    if not wm:
        return ["pdna_bank.c: meta_save() no longer calls sf_write_verified( -- "
                "check is stale, update it"]
    head = body[:wm.start()]
    if len(re.findall(r"\bf_mkdir\s*\(", head)) < 2:
        return ["pdna_bank.c: meta_save() does not f_mkdir( the Bank directory "
                "(both '/PokeDNA' and PDNA_BANK_DIR) before its own "
                "sf_write_verified( call -- the first-ever grab on a virgin card "
                "refuses with a silent beep again (BACKLOG #191a regression)"]
    return []


def run_all(box_text: str, gen12_text: str, bank_text: str) -> list[str]:
    return (check_select_gate(box_text)
            + check_move_within_wired(box_text, gen12_text)
            + check_dup_reaches_picker(gen12_text)
            + check_picker_paints_ncap(gen12_text)
            + check_move_within_moves(gen12_text)
            + check_meta_save_mkdirs(bank_text))


def main() -> int:
    if not SRC_BOX.exists() or not SRC_GEN12.exists() or not SRC_BANK.exists():
        print("SKIP (source/pdna_box.c, source/pdna_gen12.c or source/pdna_bank.c not found)")
        return 0

    box_text = SRC_BOX.read_text()
    gen12_text = SRC_GEN12.read_text()
    bank_text = SRC_BANK.read_text()

    violations = run_all(box_text, gen12_text, bank_text)
    if violations:
        print("FAIL -- shipped source has a b187 regression:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: shipped source -- F1's SELECT gate shape, F2's move_within wiring "
          "(both ends) AND its behavioural body, F3's DUPLICATE->gb_pick_box path, "
          "F3/F4's n/cap picker labels, and meta_save's Bank-directory mkdir are "
          "all present")

    tmpdir = Path(tempfile.mkdtemp(prefix="gbgridops_"))
    try:
        box_scratch = tmpdir / "pdna_box.c"
        gen12_scratch = tmpdir / "pdna_gen12.c"
        bank_scratch = tmpdir / "pdna_bank.c"

        # --- mutant 1: restore the SELECT gate (F1 regression) -----------------
        target1 = ("else if (s_cur_mode != CM_NORMAL ||\n"
                   "               (src->can_enter_move ? src->can_enter_move(box) : src_can_lift(src, box, cur))) {")
        mutated_line1 = "else if (src_can_lift(src, box, cur)) {"
        if target1 not in box_text:
            print(f"FAIL -- mutant 1 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target1!r}")
            return 1
        box_scratch.write_text(box_text.replace(target1, mutated_line1, 1))
        gen12_scratch.write_text(gen12_text)
        m1 = check_select_gate(box_scratch.read_text())
        if not m1:
            print("FAIL -- mutant 1 (restore the SELECT gate) did NOT turn check (a) "
                  "red (vacuous check)")
            return 1
        print("mutant 1 (restore the SELECT gate): correctly caught:")
        for v in m1:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- mutant 2: NULL the hook (F2 regression, table side) ---------------
        target2 = ".move_within = gb_move_within_hook,   /* BACKLOG #187/#191a, F2: within-save GB drop */"
        mutated_line2 = ".move_within = 0,"
        if target2 not in gen12_text:
            print(f"FAIL -- mutant 2 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target2!r}")
            return 1
        gen12_scratch.write_text(gen12_text.replace(target2, mutated_line2, 1))
        box_scratch.write_text(box_text)
        m2 = check_move_within_wired(box_scratch.read_text(), gen12_scratch.read_text())
        if not m2:
            print("FAIL -- mutant 2 (NULL the hook) did NOT turn check (b) red "
                  "(vacuous check)")
            return 1
        print("mutant 2 (NULL the hook): correctly caught:")
        for v in m2:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- mutant 3: restore the deny (F2 regression, call-site side) --------
        m_block = re.search(r"if \(src->scope == BOXSCOPE_GB\) \{\n"
                            r"    if \(occupied\) \{[\s\S]*?\n  \}\n",
                            box_text)
        if not m_block:
            print("FAIL -- mutant 3 target block (drop_held's GB same-scope branch) "
                  "not found (source drifted -- update this test's regex)")
            return 1
        mutated3 = box_text.replace(
            m_block.group(0),
            "if (src->scope == BOXSCOPE_GB) { snd_deny(); return recs; }\n", 1)
        box_scratch.write_text(mutated3)
        gen12_scratch.write_text(gen12_text)
        m3 = check_move_within_wired(box_scratch.read_text(), gen12_scratch.read_text())
        if not m3:
            print("FAIL -- mutant 3 (restore the deny) did NOT turn check (b) red "
                  "(vacuous check)")
            return 1
        print("mutant 3 (restore the deny): correctly caught:")
        for v in m3:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- mutant 4 (review MEDIUM): gb_move_within_hook returns true without
        # moving anything -- checks (a)-(d) alone left this green; check (e) exists
        # to catch exactly this. ---------------------------------------------
        target4 = "  return gb_move_core(box, slot, dst_box);\n}"
        mutated_line4 = "  (void)box; (void)slot; (void)dst_box; return true;\n}"
        if target4 not in gen12_text:
            print(f"FAIL -- mutant 4 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target4!r}")
            return 1
        gen12_scratch.write_text(gen12_text.replace(target4, mutated_line4, 1))
        box_scratch.write_text(box_text)
        m4 = check_move_within_moves(gen12_scratch.read_text())
        if not m4:
            print("FAIL -- mutant 4 (gb_move_within_hook returns true without "
                  "moving) did NOT turn check (e) red (vacuous check)")
            return 1
        print("mutant 4 (gb_move_within_hook returns true without moving): "
              "correctly caught:")
        for v in m4:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- mutant 5 (review fix 4, BACKLOG #191a): drop the mkdir -------------
        target5 = '  f_mkdir("/PokeDNA");\n  f_mkdir(PDNA_BANK_DIR);\n\n  uint8_t buf[META_BYTES];'
        mutated_line5 = "  uint8_t buf[META_BYTES];"
        if target5 not in bank_text:
            print(f"FAIL -- mutant 5 target not found verbatim (source drifted -- "
                  f"update this test's target string):\n{target5!r}")
            return 1
        bank_scratch.write_text(bank_text.replace(target5, mutated_line5, 1))
        m5 = check_meta_save_mkdirs(bank_scratch.read_text())
        if not m5:
            print("FAIL -- mutant 5 (drop the mkdir) did NOT turn check (f) red "
                  "(vacuous check)")
            return 1
        print("mutant 5 (drop the mkdir): correctly caught:")
        for v in m5:
            print(f"  (mutated-copy) FAIL: {v}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nhost_gb_grid_ops_test: ok (shipped source clean, all five mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
