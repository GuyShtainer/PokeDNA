#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_gb_carry_session_exit_test.py -- BACKLOG #173 review-sonnet STOP FLAG (2026-09-21):
a structural pin for the question "can a GB-origin carry (s_orig_scope == BOXSCOPE_GB)
survive gb_session_core()'s own loop and land on a Gen-3 PC screen, where the PARTY-tab
A-press collapses the origin to a plain 'PC record' via app_party_place_held(...,
s_orig_scope == BOXSCOPE_BANK, ...)?"

Traced by hand (source/pdna_gen12.c:4667-4736, both pdna_gen12_show() and
pdna_gen12_show_fused() -- the review's "nested" entry from a live Gen-3 session --
call this SAME function, source/pdna_gen12.c:4816/4894):

  for (int r; (r = pdna_box(&s)) != 0; ) {
    if (r == 2) gb_nav_from_start(m, ro);
    else if (r == 4) gb_bank_visit(m, true);
    else app_box_start_set(1);
    s = pdna_gen12_source(m);            // <- ALWAYS re-derives the SAME GB source
  }
  if (xg_clear_carry_on_gb_exit(pdna_box_carry_is_gb())) pdna_box_clear_carry();

pdna_box() (source/pdna_box.c) only ever returns 0/2/4/5 (grepped: the four
`boxoam_exit(); return N;` sites). r==5 (DOWN off a bank-shaped grid's bottom row,
gated only on `src->is_bank`, not on `bank_edge` or `!s_holding` -- pdna_box.c:3913,
the review's own anchor) is NOT special-cased above: it falls into the `else` arm,
which re-derives `s` via pdna_gen12_source(m) -- the identical GB source, never a
Gen-3 PC BoxSource. There is no other BoxSource constructor call anywhere inside this
loop body. So a still-held GB-origin carry can only ever keep re-entering the SAME
GB session's own grid through r==5 -- it cannot ride that edge onto a Gen-3 screen.
The loop's one true exit (r==0) unconditionally drops any surviving GB-origin carry
via xg_clear_carry_on_gb_exit() (source/xfer_gate.c:17: `return carry_is_gb;`,
already pinned directly by tests/host_xfergate_test.c's own
test_xg_clear_carry_on_gb_exit) BEFORE control returns to pdna_main.c's NV_GB
dispatch -- so even independent of r==5, nothing survives this function to reach a
Gen-3 PC screen.

ANSWER: the carry does NOT survive to a Gen-3 PC screen. It never leaves
gb_session_core()'s own loop through r==5 (checks (a)/(b) below), and on the loop's
real exit it is explicitly cleared (check (c), pdna_gen12.c:4734) before that exit's
own return -- self_test_mutation_detection() proves both checks have teeth by
deleting each guarantee on a scratch copy of the REAL source and showing the check
then fails.

Deliberately dumb (comment-strip + brace-depth function extraction + regex), same
idiom as tests/host_escape_gate_sites_test.py: a smarter parser could itself hide
the exact kind of refactor (a fifth `return N` value, or a second BoxSource
constructor slipped into the loop) this guards against.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN12_C = ROOT / "source" / "pdna_gen12.c"
BOX_C = ROOT / "source" / "pdna_box.c"

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


RETURN_N_RE = re.compile(r"boxoam_exit\(\);\s*return\s+(\d+);")
LOOP_HEAD_RE = re.compile(r"for\s*\(\s*int\s+r\s*;\s*\(r\s*=\s*pdna_box\(&s\)\)\s*!=\s*0\s*;\s*\)")
GEN12_SOURCE_CALL_RE = re.compile(r"pdna_gen12_source\(m\)")
CLEAR_CARRY_RE = re.compile(r"xg_clear_carry_on_gb_exit\(\s*pdna_box_carry_is_gb\(\)\s*\)")
CLEAR_CALL_RE = re.compile(r"pdna_box_clear_carry\(\)")
# Any BoxSource-shaped constructor OTHER than pdna_gen12_source -- a hard-coded
# allow-list would need updating for every new source; instead this looks for the
# one shape every such constructor in this codebase shares: `BoxSource <name> =
# <call>(...)` or `s = <call>(...)`, then checks the callee isn't pdna_gen12_source.
ASSIGN_SOURCE_RE = re.compile(r"\bs\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def pdna_box_return_codes() -> set[int]:
    """pdna_box() only returns the values it explicitly names at a
    `boxoam_exit(); return N;` site (grepped against the comment-stripped source) --
    used to confirm the loop's `else` arm truly is r==5 alone, not a silent stand-in
    for some future fifth code this test would otherwise miss."""
    text = strip_comments(BOX_C.read_text())
    return {int(m.group(1)) for m in RETURN_N_RE.finditer(text)}


def loop_never_swaps_source(lines: list[str]) -> tuple[bool, str]:
    """Check (a)+(b): inside gb_session_core()'s dispatch loop, every `s = ...(`
    assignment calls pdna_gen12_source(m) -- never a different BoxSource
    constructor -- so r==5 (the uncaptured `else` arm) can only ever re-enter the
    SAME GB session, never hand the carry to a Gen-3 PC source."""
    s, e = extract_function(lines, r"^static void gb_session_core\(")
    body = lines[s:e]
    ls, le = None, None
    for i, ln in enumerate(body):
        if LOOP_HEAD_RE.search(ln):
            ls = i
            break
    if ls is None:
        return False, "gb_session_core: loop head `for (int r; (r = pdna_box(&s)) != 0; )` not found"
    depth = 0
    seen_open = False
    for i in range(ls, len(body)):
        depth += body[i].count("{") - body[i].count("}")
        if "{" in body[i]:
            seen_open = True
        if seen_open and depth == 0:
            le = i + 1
            break
    if le is None:
        return False, "gb_session_core: loop body braces unbalanced"
    loop_body = body[ls:le]
    assigns = [m.group(1) for ln in loop_body for m in ASSIGN_SOURCE_RE.finditer(ln)]
    if not assigns:
        return False, "gb_session_core: loop body has no `s = ...(` re-derivation at all"
    bad = [c for c in assigns if c != "pdna_gen12_source"]
    if bad:
        return False, f"gb_session_core: loop body re-derives `s` from {bad}, not only pdna_gen12_source"
    return True, "ok"


def exit_clears_gb_carry(lines: list[str]) -> tuple[bool, str]:
    """Check (c): right after the loop, before the function's own unmount/return,
    a surviving GB-origin carry is unconditionally cleared: `if
    (xg_clear_carry_on_gb_exit(pdna_box_carry_is_gb())) pdna_box_clear_carry();`."""
    s, e = extract_function(lines, r"^static void gb_session_core\(")
    body = lines[s:e]
    clear_i = None
    for i, ln in enumerate(body):
        if CLEAR_CARRY_RE.search(ln):
            clear_i = i
            break
    if clear_i is None:
        return False, "gb_session_core: no xg_clear_carry_on_gb_exit(pdna_box_carry_is_gb()) call found"
    # the actual clear (pdna_box_clear_carry()) must be on the SAME line (the real
    # source is a one-line `if (...) ...;`) or within the next couple of lines.
    window = body[clear_i:clear_i + 2]
    if not any(CLEAR_CALL_RE.search(ln) for ln in window):
        return False, "gb_session_core: xg_clear_carry_on_gb_exit(...) is not paired with pdna_box_clear_carry()"
    return True, "ok"


def main() -> int:
    text = GEN12_C.read_text()
    lines = strip_comments(text).split("\n")

    codes = pdna_box_return_codes()
    check(codes == {0, 2, 4, 5}, f"pdna_box()'s own boxoam_exit();return sites now use {sorted(codes)}, "
                                 f"not {{0,2,4,5}} -- gb_session_core's loop (if/else-if on 2/4, else "
                                 f"catch-all) needs re-auditing for a new code")

    ok_a, d_a = loop_never_swaps_source(lines)
    check(ok_a, d_a)

    ok_c, d_c = exit_clears_gb_carry(lines)
    check(ok_c, d_c)

    # ---- self-mutation: both guarantees must be provably falsifiable ----------
    s, e = extract_function(lines, r"^static void gb_session_core\(")
    fn_body = lines[s:e]

    # MUT A: splice a fake alternate-source assignment into the loop ONLY (as if a
    # future edit added `s = pdna_bank_alt_source(m);` on some new loop branch) --
    # must fail (a). The insertion point is found the SAME way loop_never_swaps_
    # source() itself locates the loop, so this targets the loop body and not the
    # function's initial `BoxSource s = pdna_gen12_source(m);` (which ASSIGN_SOURCE_RE
    # also matches, being outside the loop and therefore not what this mutation means
    # to attack).
    loop_start = None
    for i in range(s, e):
        if LOOP_HEAD_RE.search(lines[i]):
            loop_start = i
            break
    if loop_start is None:
        raise AssertionError("MUT A: loop head not found in the real source")
    mut_a = list(lines)
    mut_a.insert(loop_start + 1, "    s = pdna_bank_alt_source(m);  /* MUT A */")
    ok_ma, d_ma = loop_never_swaps_source(mut_a)
    check(not ok_ma, f"MUT A (an extra BoxSource swap spliced into the loop) should "
                      f"have been caught but was not: {d_ma}")
    print(f"  MUT A demonstration -- extra BoxSource swap in the loop: {d_ma}")

    # MUT B: delete the clear-carry line entirely -- must fail (c).
    mut_b = [ln for ln in fn_body if not CLEAR_CARRY_RE.search(ln)]
    ok_mb, d_mb = exit_clears_gb_carry(mut_b)
    check(not ok_mb, f"MUT B (xg_clear_carry_on_gb_exit call deleted) should have "
                      f"been caught but was not: {d_mb}")
    print(f"  MUT B demonstration -- clear-carry line deleted: {d_mb}")

    print(f"\n{checks - len(fails)}/{checks} checks passed")
    if fails:
        for f in fails:
            print(f"  FAIL {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
