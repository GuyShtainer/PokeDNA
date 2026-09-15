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


def window_after(text: str, anchor: str, size: int = 2400) -> str:
    # 2400, not 900: the s150-2 merge put the native-cell branch (comment + the
    # gb_native_summary_open early-out) at the top of app_box_browse, which pushed the
    # commit/re-key pair to ~1,120 chars past the anchor and turned this test red on a
    # correct main. A window that stops short of the pair fails loudly (order_ok returns
    # False), never silently -- but it must be sized for the whole function.
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


def brace_block(text: str, anchor: str) -> tuple[str, int, int]:
    """Find `anchor` (must appear exactly once in `text`), then the first `{`
    at or after it, then its MATCHING `}` by brace counting (review R3:
    order_ok() only checks textual order, not nesting -- a statement hoisted
    OUT of a guarded `if { ... }` block but still textually after the anchor
    would still pass a pure order check). Returns (block_text_between_braces,
    open_index, close_index) -- both indices absolute into `text`, `open_index`
    pointing at the `{` itself and `close_index` at the matching `}`."""
    n = text.count(anchor)
    if n != 1:
        raise AssertionError(f"anchor {anchor!r} found {n} time(s), expected exactly 1")
    i = text.index(anchor)
    open_i = text.index("{", i)
    depth = 0
    j = open_i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[open_i + 1 : j], open_i, j
        j += 1
    raise AssertionError(f"no matching close brace found for anchor {anchor!r}")


def test_rekey_order_app_box_browse(text: str) -> None:
    print("== (1) app_box_browse: app_xfer_pid_rekey runs AFTER a successful app_commit_with_dex ==")
    w = window_after(text, "static bool app_box_browse(")
    check(order_ok(w, "app_commit_with_dex(rec, false, commit, block)", "app_xfer_pid_rekey(&plan)"),
          "app_box_browse: app_xfer_pid_rekey(&plan) does not appear after app_commit_with_dex(...)")
    # self-mutation: swap the two calls in the SAME window -- must now read FALSE.
    m = swapped(w, "app_commit_with_dex(rec, false, commit, block)", "app_xfer_pid_rekey(&plan)")
    check(not order_ok(m, "app_commit_with_dex(rec, false, commit, block)", "app_xfer_pid_rekey(&plan)"),
          "app_box_browse: self-mutation (order swapped) still reads as ok -- the check is vacuous")


def test_rekey_nesting(text: str, if_anchor: str, call: str, label: str) -> None:
    """review R3: order_ok() only checks textual order, not NESTING -- a rekey
    call hoisted one brace OUT of the commit's if-block but still textually
    after it would still pass a pure order check. This asserts `call` is
    actually INSIDE the `if (if_anchor) { ... }` block found by brace
    matching, then proves the check is not vacuous by moving `call`'s exact
    source line one brace out (after the block's own closing brace) and
    re-checking -- must now fail."""
    block, open_i, close_i = brace_block(text, if_anchor)
    check(call in block,
          f"{label}: {call} is not INSIDE the {if_anchor!r} if-block "
          "(order_ok() alone would miss a call hoisted one brace out but still textually after)")

    lines = block.split("\n")
    stmt_lines = [ln for ln in lines if call in ln]
    if stmt_lines:
        stmt_line = stmt_lines[0]
        mutated_lines = list(lines)
        mutated_lines.pop(mutated_lines.index(stmt_line))
        mutated_block = "\n".join(mutated_lines)
        mutated_text = (text[: open_i + 1] + mutated_block + text[close_i : close_i + 1]
                        + stmt_line.strip() + text[close_i + 1 :])
        m_block, _, _ = brace_block(mutated_text, if_anchor)
        check(call not in m_block,
              f"{label}: self-mutation (rekey hoisted one brace out) still reads as inside -- vacuous check")


def test_rekey_order_party_browse(text: str) -> None:
    print("== (2) party_browse: app_xfer_pid_rekey runs AFTER a successful app_commit_with_dex ==")
    w = window_after(text, "static bool party_browse(int start, AppCommitFn commit) {")
    check(order_ok(w, "app_commit_with_dex(rec, true, commit, g_sb1)", "app_xfer_pid_rekey(&plan)"),
          "party_browse: app_xfer_pid_rekey(&plan) does not appear after app_commit_with_dex(...)")
    m = swapped(w, "app_commit_with_dex(rec, true, commit, g_sb1)", "app_xfer_pid_rekey(&plan)")
    check(not order_ok(m, "app_commit_with_dex(rec, true, commit, g_sb1)", "app_xfer_pid_rekey(&plan)"),
          "party_browse: self-mutation (order swapped) still reads as ok -- the check is vacuous")


def test_rekey_order_daycare(text: str) -> None:
    print("== (3) day-care editable summary: WARN only, no rekey (review R1) ==")
    # BACKLOG #150 S150-6 review R1 (re-verify of F4): app_stage_sb1() is a RAM
    # stage, not a verified commit -- flush_on_exit()'s B branch drops it outright
    # ("disk untouched"), so re-keying at this site would unlink the old-key
    # record for a PID change that may never land. This site therefore must call
    # the guard (for its WARNING half) but must NEVER call app_xfer_pid_rekey.
    w = window_after(text, "if (app_xfer_pid_guard(recs[sel], out, &dplan)) {")
    check("app_xfer_pid_guard(recs[sel], out, &dplan)" in w,
          "day-care: app_xfer_pid_guard(recs[sel], out, &dplan) not found in its own window")
    check("app_xfer_pid_rekey(&dplan)" not in w,
          "day-care: app_xfer_pid_rekey(&dplan) is called here -- R1 regression "
          "(app_stage_sb1() is a RAM-only stage; flush_on_exit()'s B branch drops it, "
          "so re-keying here would unlink the old-key record for a save that may never land)")
    # self-mutation: INSERT the forbidden call right after app_stage_sb1(); -- the
    # SAME "must not contain" predicate, re-run against the mutated text, must now
    # read as violated (False), proving the check is not vacuously true.
    marker = "app_stage_sb1();"
    assert w.count(marker) == 1
    mi = w.index(marker) + len(marker)
    m = w[:mi] + " app_xfer_pid_rekey(&dplan);" + w[mi:]
    check(not ("app_xfer_pid_rekey(&dplan)" not in m),
          "day-care: self-mutation (rekey call inserted) still reads as ok -- the check is vacuous")


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
    print("== (1n) app_box_browse: app_xfer_pid_rekey is INSIDE the commit's if-block (review R3) ==")
    test_rekey_nesting(text, "if (app_commit_with_dex(rec, false, commit, block)) {",
                       "app_xfer_pid_rekey(&plan)", "app_box_browse")
    test_rekey_order_party_browse(text)
    print("== (2n) party_browse: app_xfer_pid_rekey is INSIDE the commit's if-block (review R3) ==")
    test_rekey_nesting(text, "if (app_commit_with_dex(rec, true, commit, g_sb1)) {",
                       "app_xfer_pid_rekey(&plan)", "party_browse")
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
