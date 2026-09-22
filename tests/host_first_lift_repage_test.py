#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_first_lift_repage_test.py -- structural guard for BACKLOG #197: the
first-ever UP drop into a fresh Bank (no backup-v1 DONE marker yet) must not
fail.

THE BUG (re-grepped against source/pdna_bank.c and source/pdna_box.c):
  1. drop_held's UP branch (source/pdna_box.c, the BOXSCOPE_BANK / BOXSCOPE_GB
     arm) calls `pdna_bank_prepare_native()` -> `bank_backup_v1()`
     (source/pdna_bank.c). On its FIRST-EVER success (no DONE marker yet),
     bank_backup_v1's tail sets `g_loaded = -1;` ("force a fresh page-in") --
     discarding whatever box g_bankbuf held.
  2. drop_held then runs the ident32 collision scan and the cell commit
     against the SAME `recs` pointer it already had before prepare ran --
     stale (or unset) contents from g_bankbuf, never re-paged.
  3. `src->commit()` (banksrc_commit -> box_save, source/pdna_bank.c) refuses
     outright because `g_loaded < 0` (its first line: `if (g_loaded < 0)
     return false;`) -- "bank write failed", the drop is rejected and the
     glove keeps holding, even though nothing was ever actually written.
  A second attempt succeeds only because bank_backup_v1's marker-present
  O(1) early return never touches g_loaded, so the SECOND call leaves
  g_loaded pointed at whatever box a later, unrelated page-in already set.

THE FIX (BACKLOG #197 decided shape): right after the prepare block
succeeds, re-page `recs = src->records(box);` (box_load resets g_loaded to
`box`) before the collision scan / commit read and write it.

pdna_box.c is not host-linkable (it includes <tonc.h> and "ui.h"), so this
is a text-level structural check over the real shipped source -- the same
"grep the shipped source, don't re-type your own copy of it" posture
tests/host_lift_why_gate_test.py and tests/host_escape_gate_sites_test.py
already use on this exact file. Two things are checked:

  (a) drop_held's UP-drop block (source/pdna_box.c) contains a
      `recs = src->records(box);` re-page call, textually AFTER
      `pdna_bank_prepare_native()`'s failure-return block and BEFORE both
      the ident32 collision scan's `if (!pdna_bank_serial_trusted() ||
      !s_up_scan_done) {` guard (BACKLOG #168a review D2: the one-scan-per-
      session latch; the scan itself moved into the pure
      bank_ident32_collision() core, source/bank_collision.c) and the
      `bool ok = src->commit();` line that follows it.
  (b) the failure path (`if (!pdna_bank_prepare_native())`) is unchanged:
      still returns `recs` (still holding), still logs "backup gate
      refused" -- this fix must not touch that branch at all.

Self-mutation: reverting the re-page call (BACKLOG #197's exact pre-fix
shape) must flip check (a) to FAIL -- proving the check has teeth, not just
agreeing with whatever the file happens to say.

Run directly:

    python3 tests/host_first_lift_repage_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #197).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDNA_BOX = ROOT / "source" / "pdna_box.c"

PREPARE_CALL = "if (!pdna_bank_prepare_native()) {"
REPAGE_CALL = "recs = src->records(box);"
COLLISION_LOOP = "if (!pdna_bank_serial_trusted() || !s_up_scan_done) {"
COMMIT_CALL = "bool ok = src->commit();"


def strip_comments(text: str) -> str:
    """Blank out /* ... */ and // comments but keep every newline -- a comment
    that happens to mention the right tokens must not satisfy a check the
    code itself no longer does."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    """Return the full body (signature through matching close-brace) of the
    top-level function `func_name` defined in `text`, or "" if not found.
    Brace-depth walk, robust to the function growing/shrinking as other
    lanes land -- same idiom tests/host_lift_why_gate_test.py uses.

    The return-type prefix charset includes `(` `)` so a signature carrying
    `__attribute__((noinline))` (BACKLOG #170: drop_held_up) still matches --
    plain `[\\w \\*]*` stops dead at the first paren in that attribute."""
    m = re.search(r"^\w[\w \*\(\)]*\b" + re.escape(func_name) + r"\s*\([^;]*?\)\s*\{",
                  text, re.MULTILINE)
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


def check_up_drop_repages(body: str) -> list[str]:
    """drop_held's UP-drop block: the collision scan and the commit must run
    on a FRESH page-in, not the stale `recs` the block started with."""
    failures: list[str] = []
    if PREPARE_CALL not in body:
        return [f"drop_held_up() does not call {PREPARE_CALL!r} -- BACKLOG #197's UP-drop "
                 "block has moved or been removed"]
    if COLLISION_LOOP not in body:
        return [f"drop_held_up() does not contain the ident32 collision scan "
                 f"({COLLISION_LOOP!r}) -- has it moved?"]
    if COMMIT_CALL not in body:
        return [f"drop_held_up() does not contain {COMMIT_CALL!r} -- has the commit call moved?"]

    prepare_idx = body.index(PREPARE_CALL)
    # The failure-return block closes with the FIRST `}` after the prepare call whose
    # body contains "still holding" (both this and the collision-scan refusals share
    # that comment, so anchor on the prepare call's own close instead: the next
    # occurrence of "return recs;" after prepare_idx, followed by its enclosing `}`).
    after_prepare = body[prepare_idx:]
    close_idx = after_prepare.index("return recs;")
    close_idx = after_prepare.index("}", close_idx)  # the `if (!prepare)` block's close
    scan_start = prepare_idx + close_idx

    loop_idx = body.index(COLLISION_LOOP)
    commit_idx = body.index(COMMIT_CALL)
    if not (scan_start < loop_idx < commit_idx):
        failures.append("drop_held_up()'s prepare-failure block, collision scan and "
                         "commit call are not in the expected text order -- update "
                         "this check's anchors before trusting it")
        return failures

    repage_positions = [mm.start() for mm in re.finditer(re.escape(REPAGE_CALL), body)]
    repage_between = [p for p in repage_positions if scan_start <= p < loop_idx]
    if not repage_between:
        failures.append(
            "drop_held_up()'s UP-drop block does not re-page `recs` "
            f"({REPAGE_CALL!r}) between pdna_bank_prepare_native()'s success and the "
            "ident32 collision scan / commit -- BACKLOG #197: the first-ever prepare "
            "success sets g_loaded = -1 (bank_backup_v1's own 'force a fresh page-in' "
            "tail), so the scan and src->commit() below run on stale `recs` and "
            "box_save() refuses outright because g_loaded < 0 ('bank write failed', "
            "still holding) -- the FIRST UP drop into a fresh Bank fails")
    return failures


def check_failure_path_unchanged(body: str) -> list[str]:
    """The prepare-failure branch itself must be untouched by this fix: still
    returns `recs` (still holding), still logs the backup-gate refusal."""
    if PREPARE_CALL not in body:
        return [f"drop_held_up() does not call {PREPARE_CALL!r}"]
    idx = body.index(PREPARE_CALL)
    window = body[idx:idx + 600]
    failures: list[str] = []
    if "backup gate refused" not in window:
        failures.append("drop_held_up()'s prepare-failure branch no longer logs "
                         "\"backup gate refused\" -- BACKLOG #197 must not touch this path")
    if "return recs;" not in window:
        failures.append("drop_held_up()'s prepare-failure branch no longer returns "
                         "`recs` (still holding) -- BACKLOG #197 must not touch this path")
    return failures


def revert_repage(text: str) -> str:
    """Mutated copy: BACKLOG #197's exact PRE-FIX shape -- the re-page call
    deleted, `recs` stays stale across the prepare call. BACKLOG #170 moved this
    code from drop_held's UP-branch (6-space indent, nested inside two `if`s) into
    drop_held_up's own top level (2-space indent) -- the expected text updated to
    match, same bytes otherwise."""
    old = "  recs = src->records(box);\n  /* decision 10:"
    new = "  /* decision 10:"
    assert text.count(old) == 1, ("drop_held_up()'s re-page call not found verbatim -- "
                                    "update this test's expected shape before trusting "
                                    "the mutation demonstration")
    return text.replace(old, new)


def main() -> int:
    box_text = PDNA_BOX.read_text()
    # BACKLOG #170: drop_held's UP branch (this whole check's subject) was extracted
    # into its own noinline helper, drop_held_up -- re-anchored here in the same
    # commit that moved it (a move without re-anchoring must fail loudly, not
    # silently stop checking anything).
    body = strip_comments(extract_function_body(box_text, "drop_held_up"))
    if not body:
        print("  FAIL: drop_held_up() not found in source/pdna_box.c")
        return 1

    failures = check_up_drop_repages(body) + check_failure_path_unchanged(body)
    for f in failures:
        print(f"  FAIL: {f}")
    if failures:
        print(f"host_first_lift_repage_test: {len(failures)} failure(s) on shipped source")
        return 1
    print("shipped source clean: drop_held_up()'s UP-drop block re-pages `recs` between "
          "pdna_bank_prepare_native()'s success and the ident32 collision scan / "
          "commit; the prepare-failure branch is untouched")

    # Self-mutation: revert the re-page call to BACKLOG #197's exact pre-fix shape and
    # prove check_up_drop_repages() catches it -- every run, not just when someone
    # remembers to demonstrate it by hand.
    box_body_raw = extract_function_body(box_text, "drop_held_up")
    mutated_raw = revert_repage(box_body_raw)
    mutated_body = strip_comments(mutated_raw)
    mut_failures = check_up_drop_repages(mutated_body)
    if not mut_failures:
        print("host_first_lift_repage_test: SELF-MUTATION FAILED -- reverting the "
              "re-page call (BACKLOG #197's exact pre-fix shape) was not caught")
        return 1
    print("self-mutation (BACKLOG #197 pre-fix shape) -- re-page call removed: "
          f"correctly caught:\n  (mutated-copy) FAIL: {mut_failures[0]}")

    print("host_first_lift_repage_test: ok (shipped source clean, mutation caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
