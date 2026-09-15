#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_xfer_rekey_order_test.py -- structural guard on the re-key/native-refusal
ORDER invariants (BACKLOG #150 S150-6 review F9), pure Python (no compile) --
tests/host_stack_budget_test.py-style, wired into tests/run_host_tests.py's
PY_TESTS list next to it since it, too, is a pure-Python check of source text.

Two invariants, both load-bearing and both easy to silently invert during a
future edit:

  1. D-Q3 (2026-09-15 orchestrator decision): app_xfer_pid_rekey() must run
     ONLY AFTER a verified successful commit -- never before, never
     unconditionally. At the three call sites (app_box_browse, party_browse,
     the day-care editable summary), the rekey call must appear textually
     AFTER the commit call it depends on (app_commit_with_dex() for the first
     two, app_stage_sb1() for day-care's deferred-write model) within that
     site's guarded block. Re-keying before a commit could succeed would risk
     re-keying a record whose save never actually landed.

  2. G-H6's second, independent guard: app_paste_gb_commit() must refuse a
     native `merged` (bc_is_native check) BEFORE the memcpy that lands it in
     the clipboard record -- source/pdna_main.c. A `memcpy` reordered ahead of
     the refusal defeats the whole guard silently (the refusal would still
     "run", just too late).

Each check finds its anchor by a stable, unique substring (a function
definition or one of its most distinctive statements), extracts a bounded
window of the source text after it, and asserts the two substrings appear in
that window in the right order. Self-mutation: each check is re-run against a
version of the SAME window with the two substrings swapped, and must FAIL --
proving the check is not vacuously true (a check that always passes would
never have caught the order this guards against).

Not picked up by tests/run_host_tests.py's `cc` line glob (there is nothing to
compile). Run directly:

    python3 tests/host_xfer_rekey_order_test.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDNA_MAIN = ROOT / "source" / "pdna_main.c"

FAILURES: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        FAILURES.append(msg)
        print(f"  !! FAIL: {msg}")


def window_after(text: str, anchor: str, size: int = 900) -> str:
    """The `size` characters of `text` starting at the FIRST occurrence of
    `anchor` (anchor included). Asserts the anchor is found exactly once,
    the same "one accountable site" discipline the C golden rules ask of
    call-site greps -- a stale/duplicated anchor would silently degrade this
    into checking the wrong (or an arbitrary) occurrence."""
    n = text.count(anchor)
    if n != 1:
        raise AssertionError(f"anchor {anchor!r} found {n} time(s), expected exactly 1")
    i = text.index(anchor)
    return text[i : i + size]


def order_ok(window: str, before: str, after: str) -> bool:
    """True iff both substrings are present in `window` and `before`'s first
    occurrence precedes `after`'s first occurrence."""
    bi = window.find(before)
    ai = window.find(after)
    if bi < 0 or ai < 0:
        return False
    return bi < ai


def swapped(window: str, a: str, b: str) -> str:
    """A mutated copy of `window` with the first occurrences of `a` and `b`
    exchanged -- the self-mutation vehicle. Both must be present exactly
    once each within `window` for this to be an unambiguous swap."""
    assert window.count(a) >= 1 and window.count(b) >= 1
    ia = window.index(a)
    ib = window.index(b)
    if ia < ib:
        # a ... b  ->  b ... a  (splice both substrings out, swap their order back in)
        head = window[:ia]
        mid = window[ia + len(a) : ib]
        tail = window[ib + len(b) :]
        return head + b + mid + a + tail
    else:
        head = window[:ib]
        mid = window[ib + len(b) : ia]
        tail = window[ia + len(a) :]
        return head + a + mid + b + tail


def test_rekey_order_app_box_browse(text: str) -> None:
    print("== (1) app_box_browse: app_xfer_pid_rekey runs AFTER a successful app_commit_with_dex ==")
    w = window_after(text, "static bool app_box_browse(")
    check(order_ok(w, "app_commit_with_dex(rec, false, commit, block)", "app_xfer_pid_rekey(&plan)"),
          "app_box_browse: app_xfer_pid_rekey(&plan) does not appear after app_commit_with_dex(...)")
    # self-mutation: swap the two calls in the SAME window -- must now read FALSE.
    m = swapped(w, "app_commit_with_dex(rec, false, commit, block)", "app_xfer_pid_rekey(&plan)")
    check(not order_ok(m, "app_commit_with_dex(rec, false, commit, block)", "app_xfer_pid_rekey(&plan)"),
          "app_box_browse: self-mutation (order swapped) still reads as ok -- the check is vacuous")


def test_rekey_order_party_browse(text: str) -> None:
    print("== (2) party_browse: app_xfer_pid_rekey runs AFTER a successful app_commit_with_dex ==")
    w = window_after(text, "static bool party_browse(int start, AppCommitFn commit) {")
    check(order_ok(w, "app_commit_with_dex(rec, true, commit, g_sb1)", "app_xfer_pid_rekey(&plan)"),
          "party_browse: app_xfer_pid_rekey(&plan) does not appear after app_commit_with_dex(...)")
    m = swapped(w, "app_commit_with_dex(rec, true, commit, g_sb1)", "app_xfer_pid_rekey(&plan)")
    check(not order_ok(m, "app_commit_with_dex(rec, true, commit, g_sb1)", "app_xfer_pid_rekey(&plan)"),
          "party_browse: self-mutation (order swapped) still reads as ok -- the check is vacuous")


def test_rekey_order_daycare(text: str) -> None:
    print("== (3) day-care editable summary: app_xfer_pid_rekey runs AFTER app_stage_sb1 ==")
    w = window_after(text, "if (app_xfer_pid_guard(recs[sel], out, &dplan)) {")
    check(order_ok(w, "app_stage_sb1();", "app_xfer_pid_rekey(&dplan);"),
          "day-care: app_xfer_pid_rekey(&dplan) does not appear after app_stage_sb1()")
    m = swapped(w, "app_stage_sb1();", "app_xfer_pid_rekey(&dplan);")
    check(not order_ok(m, "app_stage_sb1();", "app_xfer_pid_rekey(&dplan);"),
          "day-care: self-mutation (order swapped) still reads as ok -- the check is vacuous")


def test_native_refusal_precedes_memcpy(text: str) -> None:
    print("== (4) app_paste_gb_commit: bc_is_native(merged) refusal precedes the clipboard memcpy (G-H6) ==")
    w = window_after(text, "static bool app_paste_gb_commit(uint8_t* buf, uint32_t* len, const char* path, int idx,",
                     size=1300)
    before = "if (bc_is_native(merged)) {"
    after = "memcpy(tmp.rec, merged, 80);"
    check(order_ok(w, before, after),
          "app_paste_gb_commit: bc_is_native(merged) refusal does not precede memcpy(tmp.rec, merged, 80)")
    m = swapped(w, before, after)
    check(not order_ok(m, before, after),
          "app_paste_gb_commit: self-mutation (order swapped) still reads as ok -- the check is vacuous")


def main() -> int:
    text = PDNA_MAIN.read_text(errors="replace")

    test_rekey_order_app_box_browse(text)
    test_rekey_order_party_browse(text)
    test_rekey_order_daycare(text)
    test_native_refusal_precedes_memcpy(text)

    print()
    if FAILURES:
        print(f"host_xfer_rekey_order_test: {len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("host_xfer_rekey_order_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
