#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_xfer_items_sites_test.py -- BACKLOG #248/#249, xfer-items fix pass F4(ii): the
wiring guard.

The Opus review's own finding: the reviewer put `if (0 && ...)` in front of BOTH the
bag-insert call in gb_paste_write() and the room-ladder call in gb_bank_down_g3()
(source/pdna_gen12.c) and tests/run_host_tests.py still printed 157/157 -- the whole
item-placement feature can be deleted from its two real GBA-only call sites and no host
test goes red, because every pure-core test in this repo (host_itemladder_test.c,
host_gbbag_test.c, host_xferdown_test.c) exercises the DECISION functions directly,
never the two wiring sites that actually call them from the screens a player presses A
on. This is the same class of gap BACKLOG #171 shipped (a dead UI edge, caught only by
a shot chain, never by the host suite).

pdna_gen12.c does not compile on the host past its PDNA_GEN12_HOST guard (the GBA-only
half needs tonc/libgba this repo's pure-C host tests deliberately avoid) -- gb_paste_write
and gb_bank_down_g3 both live inside that guarded half, so the only thing that can prove
the wiring stays in place is a text-level structural check, in the same "deliberately
dumb, comment-strip + brace-depth function-body extraction" spirit as
tests/host_escape_gate_sites_test.py's own header comment: a smarter parser could itself
be fooled by exactly the kind of refactor that silently drops a call. Pure Python,
stdlib only, never `import mgba` (tests/run_host_tests.py's own RUN_HOST_TESTS
convention).

Checks:
  (a) gb_paste_write()'s (comment-stripped) body contains a real, uncommented
      `gbb_insert_and_write(` call.
  (b) gb_bank_down_g3()'s (comment-stripped) body contains a real, uncommented
      `g3gb_item_ladder(` call.
  (c) self_test_mutation_detection(): a synthetic copy of each function's body with its
      one call line blanked out makes the matching check FAIL -- proof the checker
      itself has teeth, not just that today's source happens to pass.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN12_C = ROOT / "source" / "pdna_gen12.c"

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)

GBB_INSERT_AND_WRITE_RE = re.compile(r"\bgbb_insert_and_write\(")
G3GB_ITEM_LADDER_RE = re.compile(r"\bg3gb_item_ladder\(")
# The review's OWN deletion shape: `if (0 && real_call(...))` -- the call text is still
# textually present (a naive presence-only grep would still pass), but it never runs.
# Matched on the line the call itself appears on -- this pattern is deliberately about
# what precedes the call on the SAME line, not a general "somewhere in this function"
# dead-code detector (which the review's own two-line diff did not need either).
DEAD_GUARD_RE = re.compile(r"if\s*\(\s*0\s*&&")

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip_comments(text: str) -> str:
    """Blank out /* ... */ (possibly multi-line) and // ... comments, replacing each
    with whitespace/newlines so every line's 0-based index is unchanged -- a call that
    exists only inside a comment no longer counts as "present"."""
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out_lines = []
    for ln in text.split("\n"):
        idx = ln.find("//")
        out_lines.append(ln[:idx] if idx != -1 else ln)
    return "\n".join(out_lines)


def extract_function(lines: list[str], sig_re: str) -> tuple[int, int]:
    """Return (start, end) 0-based line indices [start, end) of the function whose
    signature line matches `sig_re`, by counting braces from that line's first '{' to
    the matching close. Raises if not found or unbalanced."""
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


def has_live_call(body: list[str], pat: re.Pattern) -> bool:
    """A real, REACHABLE call: `pat` matches somewhere on a line, and that same line
    is not itself guarded by the review's own `if (0 && ...)` shape."""
    return any(pat.search(ln) and not DEAD_GUARD_RE.search(ln) for ln in body)


def blank_first_match(body: list[str], pat: re.Pattern) -> list[str]:
    """A synthetic copy of `body` with the FIRST line matching `pat` blanked out --
    simulates deleting the call outright."""
    out = list(body)
    for i, ln in enumerate(out):
        if pat.search(ln):
            out[i] = ""
            return out
    raise AssertionError("blank_first_match: no matching line found to blank")


def if0_guard_first_match(body: list[str], pat: re.Pattern) -> list[str]:
    """A synthetic copy of `body` with the FIRST line matching `pat` rewritten as the
    review's OWN `if (0 && ...)` dead-code shape -- the call text stays textually
    present (a naive presence-only grep would still pass), it just never runs. This is
    the exact regression the review actually found, not a stand-in for it."""
    out = list(body)
    for i, ln in enumerate(out):
        if pat.search(ln):
            stripped = ln.strip()
            out[i] = f"if (0 && {stripped})"
            return out
    raise AssertionError("if0_guard_first_match: no matching line found to guard")


def self_test_mutation_detection(paste_body: list[str], down_body: list[str]) -> None:
    blanked_paste = blank_first_match(paste_body, GBB_INSERT_AND_WRITE_RE)
    check(not has_live_call(blanked_paste, GBB_INSERT_AND_WRITE_RE),
          "MUT gbb_insert_and_write (deleted): blanking the call line did not make "
          "the checker see it as absent -- the checker itself has no teeth")
    guarded_paste = if0_guard_first_match(paste_body, GBB_INSERT_AND_WRITE_RE)
    check(not has_live_call(guarded_paste, GBB_INSERT_AND_WRITE_RE),
          "MUT gbb_insert_and_write (if (0 && ...)): the review's own dead-code shape "
          "did not make the checker see the call as unreachable -- a naive presence-"
          "only grep would have missed exactly what the review found")

    blanked_down = blank_first_match(down_body, G3GB_ITEM_LADDER_RE)
    check(not has_live_call(blanked_down, G3GB_ITEM_LADDER_RE),
          "MUT g3gb_item_ladder (deleted): blanking the call line did not make the "
          "checker see it as absent -- the checker itself has no teeth")
    guarded_down = if0_guard_first_match(down_body, G3GB_ITEM_LADDER_RE)
    check(not has_live_call(guarded_down, G3GB_ITEM_LADDER_RE),
          "MUT g3gb_item_ladder (if (0 && ...)): the review's own dead-code shape did "
          "not make the checker see the call as unreachable -- a naive presence-only "
          "grep would have missed exactly what the review found")


def main() -> int:
    lines = strip_comments(GEN12_C.read_text(encoding="utf-8")).splitlines()

    s, e = extract_function(lines, r"^gb_paste_write\(const GbEditMon\* mon")
    paste_body = lines[s:e]
    check(has_live_call(paste_body, GBB_INSERT_AND_WRITE_RE),
          "gb_paste_write(): no LIVE gbb_insert_and_write( call in its (comment-stripped) "
          "body -- F2's item placement is wired out or dead-guarded")

    s, e = extract_function(lines, r"^BankDownResult gb_bank_down_g3\(int dst_box")
    down_body = lines[s:e]
    check(has_live_call(down_body, G3GB_ITEM_LADDER_RE),
          "gb_bank_down_g3(): no LIVE g3gb_item_ladder( call in its (comment-stripped) "
          "body -- F2's room decision is wired out or dead-guarded")

    self_test_mutation_detection(paste_body, down_body)

    print(f"host_xfer_items_sites_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
