#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_escape_gate_sites_test.py -- BACKLOG #150 S150-3 step 5: the site guard.

pdna_box.c does not compile on the host (it needs tonc/libgba headers this repo's other
pure-C host tests deliberately avoid pulling in), so the only thing that can prove the
twelve escape-route sites from docs/briefs/s150-3-escape-gate-brief.md stay covered is a
text-level structural check: does every write path that can persist a carried/selected
80-byte record still have a gate call (`xg_native_escape_denied(` or, for the chunk LIFT,
`bc_is_native(` directly) reading BEFORE the write, by line order within the same
function body?

This is deliberately dumb (brace-depth function-body extraction + line-order regex
checks), on purpose: a smarter parser could itself hide the exact kind of refactor that
silently drops a gate. Pure Python, stdlib only, never `import mgba` (RUN_HOST_TESTS
convention, see run_host_tests.py's own header comment).

Checks (a)-(e), per the brief's step 5:
  (a) drop_held / drop_chunk / begin_select each have a gate call at a line index BEFORE
      their first 80-byte memcpy line (within that function's own body text).
  (b) the party-place call (app_party_place_held() in pdna_box.c) and the homeless-B
      memcpy line are each preceded, within a few lines, by a gate call.
  (c) app_mon_menu contains bc_is_native(rec) and a dispatch line whitelisting the
      native-cell actions (A_SUMMARY/A_MOVE/A_RELEASE/A_CANCEL -- D-Q1 added VIEW/
      A_SUMMARY to the brief's original three).
  (d) app_mon_menu_readonly contains bc_is_native(rec) in its first 10 statements.
  (e) the total count of xg_native_escape_denied( in pdna_box.c is exactly 4 (drop_held,
      drop_chunk, the party site, the homeless B-cancel).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOX_C = ROOT / "source" / "pdna_box.c"
MAIN_C = ROOT / "source" / "pdna_main.c"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def extract_function(lines: list[str], sig_re: str) -> tuple[int, int]:
    """Return (start, end) 0-based line indices [start, end) of the function whose
    signature line matches `sig_re`, by counting braces from that line's first '{'
    to the matching close. Raises if not found or unbalanced."""
    start = None
    for i, ln in enumerate(lines):
        if re.search(sig_re, ln):
            start = i
            break
    if start is None:
        raise AssertionError(f"signature not found: {sig_re}")
    depth = 0
    seen_open = False
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if "{" in lines[i]:
            seen_open = True
        if seen_open and depth == 0:
            return start, i + 1
    raise AssertionError(f"unbalanced braces for function at line {start + 1}")


MEMCPY80_RE = re.compile(r"memcpy\([^;]*,\s*80\)")
GATE_RE = re.compile(r"xg_native_escape_denied\(")
BC_NATIVE_RE = re.compile(r"bc_is_native\(")


def first_match_line(lines: list[str], start: int, end: int, pattern: re.Pattern) -> int | None:
    for i in range(start, end):
        if pattern.search(lines[i]):
            return i
    return None


def main() -> int:
    box_text = BOX_C.read_text()
    box_lines = box_text.splitlines()
    main_lines = MAIN_C.read_text().splitlines()

    # ---- (a) drop_held / drop_chunk / begin_select: gate before first 80-byte memcpy ----
    for name, sig in [
        ("drop_held", r"^static uint8_t\* drop_held\("),
        ("drop_chunk", r"^static uint8_t\* drop_chunk\(BoxSource\* src, int box, uint8_t\* recs, bool\* pfull\)"),
        ("begin_select", r"^static uint8_t\* begin_select\("),
    ]:
        s, e = extract_function(box_lines, sig)
        gate_line = first_match_line(box_lines, s, e, GATE_RE)
        bc_line = first_match_line(box_lines, s, e, BC_NATIVE_RE)
        # begin_select's chunk-LIFT guard uses bc_is_native() directly (decision 5: a lift
        # has no destination yet, so the escape predicate does not apply); the other two
        # use xg_native_escape_denied(.
        earliest_gate = min([x for x in (gate_line, bc_line) if x is not None], default=None)
        check(earliest_gate is not None, f"{name}: no gate call (xg_native_escape_denied(/bc_is_native() found in its body")
        memcpy_line = first_match_line(box_lines, s, e, MEMCPY80_RE)
        if earliest_gate is not None and memcpy_line is not None:
            check(earliest_gate < memcpy_line,
                  f"{name}: gate call (line {earliest_gate + 1}) must come BEFORE its first "
                  f"80-byte memcpy (line {memcpy_line + 1}) -- found gate AFTER or missing")
        elif memcpy_line is None:
            # begin_select's own body has no direct 80-byte memcpy in its LIFT-denial path
            # any more once the gate returns early -- still fine as long as a gate exists.
            check(earliest_gate is not None, f"{name}: expected a gate call even with no direct memcpy in-body")

    # ---- (b) party-place call + homeless-B memcpy: gate within a few lines before ----
    NEARBY = 6   # generous but still local -- not "anywhere in the file"
    party_line = None
    for i, ln in enumerate(box_lines):
        if "app_party_place_held(s_held" in ln:
            party_line = i
            break
    check(party_line is not None, "party site: app_party_place_held(s_held ... call not found in pdna_box.c")
    if party_line is not None:
        window = box_lines[max(0, party_line - NEARBY):party_line]
        check(any(GATE_RE.search(ln) for ln in window),
              f"party site: no xg_native_escape_denied( within {NEARBY} lines before "
              f"the app_party_place_held(s_held call (line {party_line + 1})")

    homeless_memcpy_line = None
    for i, ln in enumerate(box_lines):
        if "memcpy(recs + (uint32_t)fs * 80, s_held, 80)" in ln:
            homeless_memcpy_line = i
            break
    check(homeless_memcpy_line is not None, "homeless B-cancel: its memcpy(recs + fs*80, s_held, 80) line not found")
    if homeless_memcpy_line is not None:
        window = box_lines[max(0, homeless_memcpy_line - NEARBY):homeless_memcpy_line]
        check(any(GATE_RE.search(ln) for ln in window),
              f"homeless B-cancel: no xg_native_escape_denied( within {NEARBY} lines before "
              f"its memcpy (line {homeless_memcpy_line + 1})")

    # ---- (c) app_mon_menu: bc_is_native(rec) + the native dispatch whitelist ----
    s, e = extract_function(main_lines, r"^bool app_mon_menu\(uint8_t\* rec, bool is_party, bool is_bank,")
    body = main_lines[s:e]
    check(any("bc_is_native(rec)" in ln for ln in body),
          "app_mon_menu: no bc_is_native(rec) found in its body")
    # decision 9's dispatch-whitelist statement, right before the switch: must name
    # A_MOVE, A_RELEASE, A_CANCEL (the brief's literal three) -- D-Q1 (orchestrator
    # 2026-09-15) added VIEW to scope via the existing A_SUMMARY action, so the
    # statement also names A_SUMMARY; check for all four rather than "exactly three".
    dispatch_line = None
    for i, ln in enumerate(body):
        if "native &&" in ln and "act[sel] !=" in ln:
            dispatch_line = ln
            break
    check(dispatch_line is not None, "app_mon_menu: no dispatch-whitelist line (`native && act[sel] != ...`) found")
    if dispatch_line is not None:
        for action in ("A_SUMMARY", "A_MOVE", "A_RELEASE", "A_CANCEL"):
            check(action in dispatch_line, f"app_mon_menu: dispatch-whitelist line does not name {action}")
        # and must NOT whitelist any of the actions the report claims are absent/refused
        for action in ("A_ITEM", "A_LEGAL", "A_HATCH", "A_PASTE", "A_DUP", "A_TOGAME",
                        "A_EXPORT", "A_TAKEITEM", "A_GIVEITEM", "A_COPY", "A_CREATE"):
            check(action not in dispatch_line, f"app_mon_menu: dispatch-whitelist line unexpectedly names {action}")

    # ---- (d) app_mon_menu_readonly: bc_is_native(rec) in its first 10 statements ----
    s, e = extract_function(main_lines, r"^static bool app_mon_menu_readonly\(")
    # "first 10 statements" approximated as the first 10 non-blank, non-pure-comment
    # source lines of the body (skipping the signature line itself).
    stmt_lines = []
    for ln in main_lines[s + 1:e]:
        stripped = ln.strip()
        if not stripped:
            continue
        if stripped.startswith("/*") or stripped.startswith("*") or stripped.startswith("//"):
            continue
        stmt_lines.append(ln)
        if len(stmt_lines) >= 10:
            break
    check(any("bc_is_native(rec)" in ln for ln in stmt_lines),
          "app_mon_menu_readonly: no bc_is_native(rec) in its first 10 statement lines")

    # ---- (e) exact count of xg_native_escape_denied( in pdna_box.c ----
    EXPECTED_COUNT = 4   # drop_held, drop_chunk, the party site, the homeless B-cancel
    actual_count = len(GATE_RE.findall(box_text))
    check(actual_count == EXPECTED_COUNT,
          f"pdna_box.c: expected exactly {EXPECTED_COUNT} xg_native_escape_denied( call sites, found {actual_count}")

    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
