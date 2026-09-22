#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_dex_gb_cache_order_test.py -- structural guard on WHERE pdna_pick.c calls
dex_cell_art_declare_page() (BACKLOG #196), pure Python (no compile) -- tests/
host_xfer_rekey_order_test.py-style, wired into tests/run_host_tests.py's PY_TESTS
list next to it since it, too, is a pure-Python check of source text.

WHY THIS EXISTS: the per-page GB-art cache's own staleness check (poll pdna_origin_
art_invalidate_epoch(), drop the cache if it moved) is CHEAP -- but pdna_dex_set_
cell_art()'s PdnaDexCellArtPageFn contract (pdna_pick.h) promises it is called ONCE
per FULL repaint, BEFORE the per-cell paint loop, and NEVER from the bob-animation
tick. A runtime unit test cannot tell "the call happened at the right MOMENT in the
control flow" apart from "the call happened and produced the right answer" -- the
bob tick runs the SAME game state a full repaint would, so a misplaced call could
still pass every value-level check while quietly running 30x more often than the
contract promises (BACKLOG #124's own review history is full of exactly this class
of defect: a per-tick call that "worked" but was never supposed to run there at
all). Two invariants, both checked by finding the call's own line number and
comparing it against known anchor line numbers, never by pattern-matching inside a
window (BACKLOG #150's own rekey-order test uses windows because its anchors can
appear in either order; here there is exactly one call site and two fixed brackets
to check it against, so plain line numbers are the more direct, less fragile check):

  1. The ONE call to dex_cell_art_declare_page(...) in pdna_pick.c must appear
     AFTER dex_declare_page(grid, top, vis) (BACKLOG #124's own page-level hook,
     the sibling this call was modelled on) and BEFORE the following per-cell paint
     `for` loop that calls dex_cell_grid()/dex_cell_list() -- i.e. inside the same
     `if (full) { ... }` block, before the loop that actually draws the page.

  2. The call must NOT appear between the bob-animation tick's own `do {` / `} while
     (!k);` bracket (the block that re-fetches caught cells on a timer) -- the exact
     class of misplacement the brief's own mutation ("move the declaration into the
     tick -> fails") targets.

Self-mutation: each check is re-run against a synthetic version of the source text
with the call moved to the wrong place, and must FAIL -- proving the check is not
vacuously true.

Not picked up by tests/run_host_tests.py's `cc` line glob (there is nothing to
compile). Run directly:

    python3 tests/host_dex_gb_cache_order_test.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDNA_PICK = ROOT / "source" / "pdna_pick.c"

FAILURES: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        FAILURES.append(msg)


DECLARE_RE = re.compile(r"^\s*dex_cell_art_declare_page\(top, vis\);", re.M)
DEX_DECLARE_PAGE_CALL_RE = re.compile(r"^\s*dex_declare_page\(grid, top, vis\);", re.M)
PAINT_LOOP_RE = re.compile(r"for \(int i = 0; i < vis && top \+ i < g_n; i\+\+\) \{")
BOB_DO_RE = re.compile(r"^\s*do \{\s*$", re.M)
BOB_WHILE_RE = re.compile(r"^\s*\} while \(!k\);", re.M)


def find_one(pattern: re.Pattern, text: str, label: str) -> int:
    matches = list(pattern.finditer(text))
    check(len(matches) == 1, f"{label}: expected exactly 1 match, found {len(matches)}")
    return matches[0].start() if matches else -1


def run_checks(text: str, *, mutated: str = "") -> bool:
    """Returns True iff every ordering invariant holds for `text`. `mutated` is a
    label used only in failure messages, so a real run and a self-mutation run can
    share this one implementation."""
    local_failures: list[str] = []

    def lcheck(cond: bool, msg: str) -> None:
        if not cond:
            local_failures.append(msg)

    declare_pos = [m.start() for m in DECLARE_RE.finditer(text)]
    dex_declare_pos = [m.start() for m in DEX_DECLARE_PAGE_CALL_RE.finditer(text)]
    paint_loop_pos = [m.start() for m in PAINT_LOOP_RE.finditer(text)]
    bob_do_pos = [m.start() for m in BOB_DO_RE.finditer(text)]
    bob_while_pos = [m.start() for m in BOB_WHILE_RE.finditer(text)]

    lcheck(len(declare_pos) == 1, f"{mutated}exactly one dex_cell_art_declare_page(top, vis) call, found {len(declare_pos)}")
    lcheck(len(dex_declare_pos) >= 1, f"{mutated}dex_declare_page(grid, top, vis) anchor not found")
    lcheck(len(paint_loop_pos) >= 1, f"{mutated}the per-cell paint loop anchor not found")
    lcheck(len(bob_do_pos) >= 1, f"{mutated}the bob tick's own 'do {{' not found")
    lcheck(len(bob_while_pos) >= 1, f"{mutated}the bob tick's own '}} while (!k);' not found")

    if not (declare_pos and dex_declare_pos and paint_loop_pos and bob_do_pos and bob_while_pos):
        FAILURES.extend(local_failures)
        return False

    d = declare_pos[0]
    # (1) after dex_declare_page's own call, before the FIRST per-cell paint loop
    # that follows it (the one inside the same `if (full)` block).
    dp = dex_declare_pos[0]
    lcheck(d > dp, f"{mutated}dex_cell_art_declare_page must come AFTER dex_declare_page(grid, top, vis)")
    following_loops = [p for p in paint_loop_pos if p > dp]
    lcheck(bool(following_loops), f"{mutated}no paint loop found after dex_declare_page")
    if following_loops:
        loop = min(following_loops)
        lcheck(d < loop, f"{mutated}dex_cell_art_declare_page must come BEFORE the per-cell paint loop")

    # (2) never inside the bob tick's own do{}while(!k) bracket -- check every
    # do/while pair (there is more than one `do { ... } while` idiom in this file,
    # e.g. the input-wait helper near the top) that actually contains the paint
    # loop's OWN bob-refresh marker, not just any do/while in the file.
    for do_pos in bob_do_pos:
        closing = [w for w in bob_while_pos if w > do_pos]
        if not closing:
            continue
        end = min(closing)
        if do_pos < d < end:
            local_failures.append(f"{mutated}dex_cell_art_declare_page must NEVER appear inside a do{{...}} while (!k); bob-tick bracket "
                                   f"(found between offsets {do_pos} and {end})")

    if local_failures:
        FAILURES.extend(local_failures)
        return False
    return True


def main() -> int:
    text = PDNA_PICK.read_text()

    ok_real = run_checks(text)
    print(f"(1) real source ordering: {'ok' if ok_real else 'FAILED (see above)'}")

    # Self-mutation A: move the call INTO the bob tick (right after its own `do {`)
    # and remove it from its real spot -- must FAIL.
    declare_line_match = DECLARE_RE.search(text)
    bob_do_match = BOB_DO_RE.search(text)
    assert declare_line_match and bob_do_match, "anchors must exist for the mutation to be meaningful"
    call_text = declare_line_match.group(0)
    mutated_a = text[: declare_line_match.start()] + text[declare_line_match.end():]
    # re-find the (now-shifted) do{ position in the mutated text and splice the call
    # in right after it.
    bob_do_in_mutated = BOB_DO_RE.search(mutated_a)
    assert bob_do_in_mutated is not None
    insert_at = bob_do_in_mutated.end()
    mutated_a = mutated_a[:insert_at] + "\n      " + call_text + mutated_a[insert_at:]
    before = len(FAILURES)
    ok_mut_a = run_checks(mutated_a, mutated="[mutant A] ")
    caught_a = not ok_mut_a and len(FAILURES) > before
    print(f"(2) mutant A (call moved into the bob tick): {'caught (fails, as required)' if caught_a else 'NOT CAUGHT -- test is vacuous'}")

    # Self-mutation B: move the call to AFTER the paint loop (still inside `if
    # (full)`, but past the loop it must precede) -- must FAIL.
    paint_loop_match = PAINT_LOOP_RE.search(text, dex_declare_pos_end(text))
    assert paint_loop_match is not None
    # find the loop's closing brace by simple brace counting from its opening `{`.
    brace_start = text.index("{", paint_loop_match.end() - 1)
    depth = 0
    i = brace_start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    loop_end = i + 1
    mutated_b = text[: declare_line_match.start()] + text[declare_line_match.end():]
    # re-locate the loop end in the mutated text (call removal shifted offsets).
    shift = declare_line_match.end() - declare_line_match.start()
    loop_end_mut = loop_end - shift if loop_end > declare_line_match.start() else loop_end
    mutated_b = mutated_b[:loop_end_mut] + "\n      " + call_text + mutated_b[loop_end_mut:]
    before = len(FAILURES)
    ok_mut_b = run_checks(mutated_b, mutated="[mutant B] ")
    caught_b = not ok_mut_b and len(FAILURES) > before
    print(f"(3) mutant B (call moved after the paint loop): {'caught (fails, as required)' if caught_b else 'NOT CAUGHT -- test is vacuous'}")

    real_ok = ok_real
    mutants_caught = caught_a and caught_b
    overall_ok = real_ok and mutants_caught
    print(f"\n{'ALL OK' if overall_ok else 'FAILED'} -- real ordering {'holds' if real_ok else 'BROKEN'}, "
          f"both mutants {'caught' if mutants_caught else 'NOT all caught'}")
    return 0 if overall_ok else 1


def dex_declare_pos_end(text: str) -> int:
    m = DEX_DECLARE_PAGE_CALL_RE.search(text)
    return m.end() if m else 0


if __name__ == "__main__":
    sys.exit(main())
