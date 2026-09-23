#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_clip_session_clear_test.py -- BACKLOG #239 fix-pass structural pin: the mon and
held-item clipboards (g_clip, g_item_clip/g_item_held) are file-scope statics that
NOTHING else in source/pdna_main.c ever clears. main()'s loop is
`for(;;){ if (browse_pick(...)) view_save(path); }`, so two saves meet SEQUENTIALLY in
one power cycle -- COPY in save A, close it, open save B, PASTE/PASTE HERE/RO_PASTE/
day-care PUT IN in save B, and the mon (or held item) crosses saves with no Bank in the
middle, and is never removed from save A either (a clone, not a move).

The fix is two lines inside view_save(), placed ABOVE the `#ifdef PDNA_DELTA` block so
the delta build's own fused-clip seeds (clip_copy_from at :11228/:11262/:11337, all
`#ifdef PDNA_DELTA`-gated) still run afterward and no existing shot chain changes:

    g_clip.occupied = false; g_clip.from_gb = false;
    g_item_held = false; g_item_clip = 0;

This is pure C with no host half (pdna_main.c does not dual-compile), so -- same idiom
as tests/host_gb_carry_session_exit_test.py -- this pins the guarantee STRUCTURALLY:
comment-strip, extract view_save() by brace depth, confirm both clearing statements
appear before the function's first `#ifdef PDNA_DELTA` line. Delete either clearing
line and the check must fail; two independent mutations prove that.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN_C = ROOT / "source" / "pdna_main.c"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out = []
    for ln in text.split("\n"):
        idx = ln.find("//")
        out.append(ln[:idx] if idx != -1 else ln)
    return "\n".join(out)


def extract_function(lines: list[str], sig_re: str) -> tuple[int, int]:
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


CLIP_CLEAR_RE = re.compile(r"g_clip\.occupied\s*=\s*false")
ITEM_CLEAR_RE = re.compile(r"g_item_held\s*=\s*false")
DELTA_IFDEF_RE = re.compile(r"^\s*#ifdef\s+PDNA_DELTA\b")


def clip_and_item_cleared_above_delta(lines: list[str]) -> tuple[bool, str]:
    """view_save() must clear both g_clip.occupied and g_item_held, and both clears
    must appear strictly before the function's first `#ifdef PDNA_DELTA` line."""
    s, e = extract_function(lines, r"^static void view_save\(")
    body = lines[s:e]
    delta_i = None
    for i, ln in enumerate(body):
        if DELTA_IFDEF_RE.search(ln):
            delta_i = i
            break
    if delta_i is None:
        return False, "view_save: no #ifdef PDNA_DELTA line found at all"
    clip_i = next((i for i, ln in enumerate(body) if CLIP_CLEAR_RE.search(ln)), None)
    item_i = next((i for i, ln in enumerate(body) if ITEM_CLEAR_RE.search(ln)), None)
    if clip_i is None:
        return False, "view_save: no `g_clip.occupied = false` statement found"
    if item_i is None:
        return False, "view_save: no `g_item_held = false` statement found"
    if clip_i >= delta_i:
        return False, f"view_save: g_clip clear at body line {clip_i} is not above #ifdef PDNA_DELTA at {delta_i}"
    if item_i >= delta_i:
        return False, f"view_save: g_item_held clear at body line {item_i} is not above #ifdef PDNA_DELTA at {delta_i}"
    return True, "ok"


def main() -> int:
    text = MAIN_C.read_text()
    lines = strip_comments(text).split("\n")

    ok, d = clip_and_item_cleared_above_delta(lines)
    check(ok, d)

    # ---- self-mutation: both guarantees must be provably falsifiable ----------
    s, e = extract_function(lines, r"^static void view_save\(")
    fn_body = lines[s:e]

    # MUT A: delete the g_clip clear line -- must fail.
    mut_a = [ln for ln in fn_body if not CLIP_CLEAR_RE.search(ln)]
    ok_ma, d_ma = clip_and_item_cleared_above_delta(mut_a)
    check(not ok_ma, f"MUT A (g_clip.occupied clear deleted) should have been "
                      f"caught but was not: {d_ma}")
    print(f"  MUT A demonstration -- g_clip clear deleted: {d_ma}")

    # MUT B: delete the g_item_held clear line -- must fail.
    mut_b = [ln for ln in fn_body if not ITEM_CLEAR_RE.search(ln)]
    ok_mb, d_mb = clip_and_item_cleared_above_delta(mut_b)
    check(not ok_mb, f"MUT B (g_item_held clear deleted) should have been "
                      f"caught but was not: {d_mb}")
    print(f"  MUT B demonstration -- g_item_held clear deleted: {d_mb}")

    print(f"\n{checks - len(fails)}/{checks} checks passed")
    if fails:
        for f in fails:
            print(f"  FAIL {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
