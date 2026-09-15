#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_escape_gate_sites_test.py -- BACKLOG #150 S150-3 step 5 + review F1/F3: the site guard.

pdna_box.c does not compile on the host (it needs tonc/libgba headers this repo's other
pure-C host tests deliberately avoid pulling in), so the only thing that can prove the
escape-route sites from docs/briefs/s150-3-escape-gate-brief.md (plus review F1's
fourteenth route) stay covered is a text-level structural check: does every write path
that can persist a carried/selected 80-byte record still have a gate call
(`xg_native_escape_denied(` or, where the predicate does not apply, `bc_is_native(`
directly) reading BEFORE the write, by line order within the same function body?

This is deliberately dumb (comment-strip + brace-depth function-body extraction +
line-order regex checks), on purpose: a smarter parser could itself hide the exact kind
of refactor that silently drops a gate. Pure Python, stdlib only, never `import mgba`
(RUN_HOST_TESTS convention, see run_host_tests.py's own header comment).

Review F3 (2026-09-15) found two holes in the FIRST version of this file:
  - it never stripped comments, so a gate call sitting only in a /* ... */ block (dead
    prose, not real code) still counted as present;
  - check (a) took `min(gate_line, bc_line)` across BOTH patterns for every site, so a
    site whose REAL pattern (e.g. drop_held's own `xg_native_escape_denied(`) was
    deleted, or moved to AFTER its memcpy, could still pass by accident if some
    UNRELATED `bc_is_native(` call happened to sit earlier in the same body (begin_select
    has one from decision 5; nothing stopped it from masking a deleted drop_held gate
    if the two functions were ever merged or reordered).
Both are fixed here: `strip_comments()` blanks `/*...*/` and `//` text but keeps every
line's own line number (so line-order checks after stripping still point at real source
lines), and each site now checks its OWN specific pattern only -- never a min() over two.
A permanent self-mutation harness (`self_test_mutation_detection()`) proves the checker
still has teeth: it takes the REAL stripped body text, applies the exact two mutations
review F3 asked for (MUT A: delete begin_select's guard; MUT F: move drop_held's gate
block after its five memcpys) to an IN-MEMORY copy, and asserts the per-site check
function reports failure on the mutated copy -- every run, not just when someone
remembers to run the demonstration by hand.

Checks:
  (a) drop_held / drop_chunk: `xg_native_escape_denied(` at a line index BEFORE the
      first 80-byte memcpy line, in the (comment-stripped) function body.
  (a') begin_select: `bc_is_native(` at a line index BEFORE its `memcpy(s_ch_rec[i]`
      copy-into-chunk loop (decision 5 -- a lift has no destination yet, so the escape
      predicate does not apply here).
  (a'') review F1: box_set_held contains `bc_is_native(` before its own
      `memcpy(rec, out, 80)`; item_home contains `bc_is_native(` in its scan (no write
      of its own, so no before/after ordering to check -- presence only).
  (b) the party-place call (app_party_place_held() in pdna_box.c) and the homeless-B
      memcpy line are each preceded, within a few lines, by a gate call.
  (c) app_mon_menu contains bc_is_native(rec) and a dispatch line whitelisting the
      native-cell actions (A_SUMMARY/A_MOVE/A_RELEASE/A_CANCEL -- D-Q1 added VIEW/
      A_SUMMARY to the brief's original three).
  (d) app_mon_menu_readonly (review F4 shape): contains `bc_is_native(rec)` (the
      `ro_native` computation) and a dispatch line whitelisting RO_VIEW/RO_CANCEL only.
  (e) the total count of xg_native_escape_denied( in pdna_box.c is exactly 4 (drop_held,
      drop_chunk, the party site, the homeless B-cancel) -- review F1/F2 use
      bc_is_native() directly, not this predicate, so this count is unchanged by them.
  (f) self_test_mutation_detection(): MUT A and MUT F both make the relevant per-site
      check FAIL, on synthetic mutated copies, every run.
  (j) BACKLOG #171 (lane s150-4-5b): every `s_tab_focus = 1;` site inside pdna_box()'s
      own `if (s_holding) { ... }` carrying block that is ALSO gated on `!src->is_bank`
      (on the same line -- this file's own dense single-line if/else-if style) carries
      `|| src->bank_edge` on that same line, and there is exactly ONE such site --
      without the clause a GB source (always is_bank AND bank_edge, never is_bank
      alone) can never enter tab focus while holding a lifted mon, so `UP` from row 0
      while carrying does nothing at all (found live in mGBA: every frame after the
      tap was pixel-identical to the one before it). MUT I reverts the real site to
      its pre-fix `!src->is_bank`-only form and asserts the checker catches it.
  (k) BACKLOG #171/#171b review F1 (lane s150-4-5b): begin_select's refusal branch
      (`} else { snd_deny(); *pfull = true; }`) sets *pfull = true -- one of this
      repo's four named partial-repaint trap classes: a REFUSED lift after
      gb_pick_origin's full-screen picker painted leaves that picker's bitmap on
      screen, un-erased, until the next need_full repaint; a bare `else snd_deny();`
      left that to chance (found live in mGBA: the refused-origin-prompt frame stayed
      on screen with box icons drawn over it). MUT J reverts the real branch to its
      pre-fix bare form and asserts the checker catches it.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOX_C = ROOT / "source" / "pdna_box.c"
MAIN_C = ROOT / "source" / "pdna_main.c"
BANK_C = ROOT / "source" / "pdna_bank.c"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


# ---- comment stripping (review F3): blank /*...*/ and // text, keep line count -------
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank out /* ... */ (possibly multi-line) and // ... comments, replacing each
    with whitespace/newlines so every line's 0-based index is unchanged -- a gate call
    or a memcpy that exists only inside a comment no longer counts as "present"."""
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out_lines = []
    for ln in text.split("\n"):
        idx = ln.find("//")
        out_lines.append(ln[:idx] if idx != -1 else ln)
    return "\n".join(out_lines)


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
# BACKLOG #150 S150-8 review F5: module-level so both the real check (j) and its
# self-mutation demonstration (MUT I) share one pattern.
CONVERTED_TRUE_RE = re.compile(r"\bconverted\s*=\s*true\b")
CHUNK_COPY_RE = re.compile(r"memcpy\(s_ch_rec\[i\]")
GATE_RE = re.compile(r"xg_native_escape_denied\(")
BC_NATIVE_RE = re.compile(r"bc_is_native\(")
# BACKLOG #150 S150-4: both start_carry() sites reachable with a BOXSCOPE_GB source
# must be preceded by this exact call -- module-level so both the real check (g) and
# its self-mutation demonstration (MUT G) share one pattern.
SRC_CAN_LIFT_CUR_RE = re.compile(r"src_can_lift\(src, box, cur\)")
COMBINED_GUARD_RE = re.compile(
    r"if\s*\(\s*!src_can_lift\(src, box, cur\)\s*\|\|\s*!start_carry\(src, recs, box, cur\)\s*\)")
# REVIEW F1: pdna_bank_next_serial() must call meta_load( BEFORE meta_save( -- g_meta/
# g_bank_serial are populated ONLY by pdna_bank_show(); a GB-grid lift (this lane's
# whole reason for pdna_bank_next_serial existing) runs before that ever happens, so
# without this the first UP lift of a session persists zeroed box names/wallpapers and
# re-issues serial 1.
META_LOAD_RE = re.compile(r"\bmeta_load\(")
META_SAVE_RE = re.compile(r"\bmeta_save\(")

# REVIEW F2: nothing pinned commit-before-delete in drop_held's UP branch -- the
# reviewer swapped release_up before commit() and ungated it, and every OTHER check
# in this file still passed. These four regexes and up_order_facts() are shared by
# the real check (i) and its self-mutation demonstration (MUT H).
COMMIT_RE      = re.compile(r"\bok = src->commit\(\)")
RELEASE_UP_RE  = re.compile(r"s_xfer_peer->release_up\(")
ZEROBACK_RE    = re.compile(r"memset\(recs \+ \(uint32_t\)cur \* 80, 0, 80\)")
RETURN_RECS_RE = re.compile(r"^\s*return recs;")

# BACKLOG #171 (lane s150-4-5b): the dead-carry site guard. `TAB1_ASSIGN_RE` matches
# only the literal `s_tab_focus = 1;` assignment (never the two ternary forms
# `s_tab_focus = src->is_bank ? 2 : 1;` / `(s_tab_focus > 0) ? s_tab_focus - 1 : 2` --
# those always assign regardless of is_bank, so they cannot be the dead-carry shape).
TAB1_ASSIGN_RE     = re.compile(r"s_tab_focus\s*=\s*1\s*;")
IS_BANK_GUARD_RE   = re.compile(r"!src->is_bank")
BANK_EDGE_CLAUSE_RE = re.compile(r"src->bank_edge")
HOLDING_BLOCK_RE    = re.compile(r"if\s*\(\s*s_holding\s*\)\s*\{")

# BACKLOG #171/#171b review F1 (lane s150-4-5b): begin_select's refusal branch must
# set *pfull = true -- shared by the real check (k) and its self-mutation
# demonstration (MUT J).
BEGIN_SELECT_REFUSAL_RE = re.compile(r"else\s*\{\s*snd_deny\(\);\s*\*pfull\s*=\s*true;\s*\}")

# BACKLOG #142 (lane b142): the entry-time cursor-arrival hint's own `st == 1 &&
# !s_holding` site (pdna_box()'s body OUTSIDE the s_holding carrying block above --
# this one runs on a fresh, non-carrying arrival) must never send an is_bank source
# to the SAVE tab. `SAVE_TAB_TERNARY_RE` matches the exact pre-fix shape,
# `s_tab_focus = src->is_bank ? 2 : 1;` -- an is_bank grid's tab 1 is the inert
# "(BANK)" label, so a Game Boy session (the only source that reaches this site with
# is_bank true -- the real Bank never gets st==1, see the two app_box_start_set(1)
# call sites, both captioned "bank dropped off the bottom -> PC opens/tabs") landed
# on SAVE, one A away from ending the session.
ARRIVAL_ST1_RE       = re.compile(r"st == 1 && !s_holding")
SAVE_TAB_TERNARY_RE  = re.compile(r"is_bank\s*\?\s*2\s*:\s*1")


def arrival_never_saves_check(pdna_box_body: list[str]) -> tuple[bool, list[str]]:
    """BACKLOG #142: exactly one `st == 1 && !s_holding` site must exist in
    pdna_box()'s own body, and none of its matching lines may carry the
    `is_bank ? 2 : 1` ternary that sends an is_bank arrival to the SAVE tab.
    Shared by the real check (l) and its self-mutation demonstration (MUT N)."""
    sites = [ln for ln in pdna_box_body if ARRIVAL_ST1_RE.search(ln)]
    bad = [ln for ln in sites if SAVE_TAB_TERNARY_RE.search(ln)]
    return (len(sites) == 1 and not bad), sites


def carrying_block(pdna_box_body: list[str]) -> list[str]:
    """The `if (s_holding) { ... }` sub-block inside pdna_box()'s own body (the "MOVE
    MODE (mon-in-hand)" section) -- found by a direct line scan for its own opening
    brace, then brace-counted to its close, the same idiom extract_function() uses but
    starting from a known index instead of a fresh regex search over the whole file."""
    start = None
    for i, ln in enumerate(pdna_box_body):
        if HOLDING_BLOCK_RE.search(ln):
            start = i
            break
    if start is None:
        raise AssertionError("pdna_box(): `if (s_holding) {` block not found")
    depth = 0
    for i in range(start, len(pdna_box_body)):
        depth += pdna_box_body[i].count("{") - pdna_box_body[i].count("}")
        if depth == 0 and i > start:
            return pdna_box_body[start:i + 1]
    raise AssertionError("pdna_box(): unbalanced braces in the s_holding carrying block")


def bank_edge_sites_check(block: list[str]) -> tuple[bool, list[str]]:
    """BACKLOG #171: every `s_tab_focus = 1;` site in `block` that is ALSO gated on
    `!src->is_bank` (on the SAME line -- this codebase's own dense single-line
    if/else-if style) must carry `|| src->bank_edge` on that same line, or a GB source
    (always is_bank AND bank_edge, never is_bank alone -- pdna_gen12_source()) can
    never enter tab focus while holding, so `UP` from row 0 while carrying does
    nothing -- the exact dead-carry bug lane s150-4-5b found live in mGBA (every frame
    after the tap pixel-identical to the one before it). Shared by the real check (j)
    and its self-mutation demonstration (MUT I). Returns (all_ok, per_site_details)."""
    sites = [i for i, ln in enumerate(block) if TAB1_ASSIGN_RE.search(ln)]
    details = []
    all_ok = True
    for i in sites:
        ln = block[i]
        guarded = bool(IS_BANK_GUARD_RE.search(ln))
        has_clause = bool(BANK_EDGE_CLAUSE_RE.search(ln))
        if guarded and not has_clause:
            all_ok = False
            details.append(f"line {i + 1}: guarded on !src->is_bank but missing "
                            f"`|| src->bank_edge` -- a GB source can never enter tab "
                            f"focus at this site while holding")
        else:
            details.append(f"line {i + 1}: ok ({'guarded+clause' if guarded else 'unguarded, no clause needed'})")
    return all_ok, details

def up_order_facts(lines, start, end):           # shared by the real check AND MUT H
    c = first_match_line(lines, start, end, COMMIT_RE)
    r = first_match_line(lines, start, end, RELEASE_UP_RE)
    z = first_match_line(lines, start, end, ZEROBACK_RE)
    if c is None: return False, "drop_held: no `ok = src->commit()` line in the UP branch"
    if r is None: return False, "drop_held: no `s_xfer_peer->release_up(` call"
    if z is None: return False, "drop_held: no zero-back memset of the destination cell"
    if not c < r: return False, (f"drop_held: src->commit() (line {c+1}) does NOT come before "
                                 f"release_up() (line {r+1}) -- the Game Boy save would lose the "
                                 f"mon before the Bank has it")
    if not c < z < r: return False, (f"drop_held: the zero-back memset (line {z+1}) is not between "
                                     f"commit() ({c+1}) and release_up() ({r+1})")
    if first_match_line(lines, z, r, RETURN_RECS_RE) is None:
        return False, (f"drop_held: no `return recs;` between the zero-back memset (line {z+1}) and "
                       f"release_up() (line {r+1}) -- release_up is not dominated by the "
                       f"commit-failure early-out")
    return True, "ok"


# REVIEW F2 (BACKLOG #150 S150-7): nothing pinned accept_down() (the persist) before
# app_bank_clear_slots() (the Bank consume) in bank_down_exact() -- the reviewer
# inverted them and every other check stayed green (D7/D12's whole point: the Bank
# copy must not be zeroed until the Game Boy save actually has the mon). Modelled
# byte-for-byte on up_order_facts() above -- same shared-function-for-real-check-and-
# MUT posture (review F3's point in this same file).
ACCEPT_DOWN_RE = re.compile(r"s_xfer_peer->accept_down\(")
CONSUME_RE = re.compile(r"app_bank_clear_slots\(")


def down_order_facts(lines, start, end):        # shared by the real check AND MUT I
    a = first_match_line(lines, start, end, ACCEPT_DOWN_RE)
    c = first_match_line(lines, start, end, CONSUME_RE)
    if a is None: return False, "bank_down_exact: no `s_xfer_peer->accept_down(` call"
    if c is None: return False, "bank_down_exact: no `app_bank_clear_slots(` call"
    if not a < c: return False, (f"bank_down_exact: accept_down() (line {a+1}, the persist) does NOT "
                                 f"come before app_bank_clear_slots() (line {c+1}, the Bank consume) "
                                 f"-- the Bank's own copy could be zeroed before the Game Boy save "
                                 f"has the mon")
    return True, "ok"


def first_match_line(lines: list[str], start: int, end: int, pattern: re.Pattern) -> int | None:
    for i in range(start, end):
        if pattern.search(lines[i]):
            return i
    return None


def gate_before_pattern(lines: list[str], start: int, end: int, gate_pat: re.Pattern,
                         write_pat: re.Pattern, name: str) -> tuple[bool, str]:
    """The check function itself, factored out so both the real run AND the
    self-mutation harness call the exact same logic (review F3's point: the checker
    that runs on real source and the checker whose teeth get proven must be identical,
    not a hand-verified-once demonstration)."""
    gate_line = first_match_line(lines, start, end, gate_pat)
    write_line = first_match_line(lines, start, end, write_pat)
    if gate_line is None:
        return False, f"{name}: no {gate_pat.pattern} call found in its (comment-stripped) body"
    if write_line is None:
        return True, f"{name}: gate present, no matching write pattern found (nothing to dominate)"
    if gate_line >= write_line:
        return False, (f"{name}: gate call (line {gate_line + 1}) does not come BEFORE its "
                        f"write (line {write_line + 1})")
    return True, f"{name}: ok (gate line {gate_line + 1} < write line {write_line + 1})"


def main() -> int:
    box_lines = strip_comments(BOX_C.read_text()).splitlines()
    box_text_stripped = "\n".join(box_lines)
    main_lines = strip_comments(MAIN_C.read_text()).splitlines()
    bank_lines = strip_comments(BANK_C.read_text()).splitlines()

    # ---- (h) REVIEW F1: pdna_bank_next_serial() calls meta_load( before meta_save( ----
    s, e = extract_function(bank_lines, r"^uint32_t pdna_bank_next_serial\(void\)")
    ns_body = bank_lines[s:e]
    load_line = first_match_line(ns_body, 0, len(ns_body), META_LOAD_RE)
    save_line = first_match_line(ns_body, 0, len(ns_body), META_SAVE_RE)
    check(load_line is not None,
          "pdna_bank_next_serial(): no meta_load( call in its (comment-stripped) body")
    check(save_line is not None,
          "pdna_bank_next_serial(): no meta_save( call in its (comment-stripped) body")
    if load_line is not None and save_line is not None:
        check(load_line < save_line,
              f"pdna_bank_next_serial(): meta_load( (line {load_line + 1}) does not come "
              f"BEFORE meta_save( (line {save_line + 1}) -- a GB-grid lift (which runs "
              f"before pdna_bank_show() ever populates g_meta/g_bank_serial) would zero "
              f"bank.meta and re-issue a spent serial")

    # ---- (a) drop_held / drop_chunk: xg_native_escape_denied( before first 80-byte memcpy ----
    for name, sig in [
        ("drop_held", r"^static uint8_t\* drop_held\("),
        ("drop_chunk", r"^static uint8_t\* drop_chunk\(BoxSource\* src, int box, uint8_t\* recs, bool\* pfull\)"),
    ]:
        s, e = extract_function(box_lines, sig)
        ok, detail = gate_before_pattern(box_lines, s, e, GATE_RE, MEMCPY80_RE, name)
        check(ok, detail)

    # ---- (a''''=j) BACKLOG #150 S150-8 review F5: `bool converted = true;` at drop_held's
    # top would satisfy check (a) above (it never even LOOKS at the gate's condition, only
    # its line position relative to the first 80-byte memcpy) while unconditionally
    # bypassing the whole native-escape gate. Two independent checks close that hole:
    #   - the ONE xg_native_escape_denied( call in drop_held must read
    #     `if (!converted && xg_native_escape_denied(` -- not a bare, unconditional gate;
    #   - `converted = true` must be assigned exactly once, and only within 3 lines after
    #     a line that names BANK_DOWN_CONVERTED (the dispatch result check that is the
    #     ONLY legitimate reason `converted` may ever become true).
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    dh_body = box_lines[s:e]
    gate_idx = [i for i in range(len(dh_body)) if GATE_RE.search(dh_body[i])]
    check(len(gate_idx) == 1 and "!converted &&" in dh_body[gate_idx[0]],
          "drop_held: the one escape gate must read "
          "`if (!converted && xg_native_escape_denied(` -- found: " +
          (dh_body[gate_idx[0]].strip() if len(gate_idx) == 1 else f"{len(gate_idx)} gate call(s)"))
    asg_idx = [i for i, ln in enumerate(dh_body) if CONVERTED_TRUE_RE.search(ln)]
    check(len(asg_idx) == 1 and
          any("BANK_DOWN_CONVERTED" in dh_body[j] for j in range(max(0, asg_idx[0] - 3), asg_idx[0])),
          "drop_held: `converted = true` must be assigned exactly once, immediately after "
          "a BANK_DOWN_CONVERTED test -- found at line(s) " +
          ", ".join(str(i + 1) for i in asg_idx))

    # ---- (a') begin_select: bc_is_native( before the copy-into-chunk loop ----
    s, e = extract_function(box_lines, r"^static uint8_t\* begin_select\(")
    ok, detail = gate_before_pattern(box_lines, s, e, BC_NATIVE_RE, CHUNK_COPY_RE, "begin_select")
    check(ok, detail)

    # ---- (a'') review F1: box_set_held + item_home ----
    s, e = extract_function(box_lines, r"^static bool box_set_held\(")
    ok, detail = gate_before_pattern(box_lines, s, e, BC_NATIVE_RE, MEMCPY80_RE, "box_set_held")
    check(ok, detail)

    # ---- (a''') review F2: drop_held's SWAP DESTINATION guard. Two-term pattern on
    # purpose -- a bare `bc_is_native(recs + (uint32_t)cur * 80)` is masked by the
    # `occupied` computation higher in the same body (S150-2, G-H2).
    F2_SWAP_RE = re.compile(r"bc_is_native\([^;]*\)\s*&&\s*!bc_is_native\(s_held\)")
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    ok, detail = gate_before_pattern(box_lines, s, e, F2_SWAP_RE,
                                     re.compile(r"memcpy\(occ, recs"), "drop_held SWAP dest (F2)")
    check(ok, detail)

    s, e = extract_function(box_lines, r"^static int item_home\(")
    check(first_match_line(box_lines, s, e, BC_NATIVE_RE) is not None,
          "item_home: no bc_is_native( found in its (comment-stripped) body")

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
          "app_mon_menu: no bc_is_native(rec) found in its (comment-stripped) body")
    dispatch_line = None
    for i, ln in enumerate(body):
        if "native &&" in ln and "act[sel] !=" in ln:
            dispatch_line = ln
            break
    check(dispatch_line is not None, "app_mon_menu: no dispatch-whitelist line (`native && act[sel] != ...`) found")
    if dispatch_line is not None:
        for action in ("A_SUMMARY", "A_MOVE", "A_RELEASE", "A_CANCEL"):
            check(action in dispatch_line, f"app_mon_menu: dispatch-whitelist line does not name {action}")
        for action in ("A_ITEM", "A_LEGAL", "A_HATCH", "A_PASTE", "A_DUP", "A_TOGAME",
                        "A_EXPORT", "A_TAKEITEM", "A_GIVEITEM", "A_COPY", "A_CREATE"):
            check(action not in dispatch_line, f"app_mon_menu: dispatch-whitelist line unexpectedly names {action}")

    # ---- (d) app_mon_menu_readonly (review F4 shape): whitelist, not blunt early return ----
    s, e = extract_function(main_lines, r"^static bool app_mon_menu_readonly\(")
    body = main_lines[s:e]
    check(any("bc_is_native(rec)" in ln for ln in body),
          "app_mon_menu_readonly: no bc_is_native(rec) (the ro_native computation) found "
          "in its (comment-stripped) body")
    ro_dispatch_line = None
    for i, ln in enumerate(body):
        if "ro_native &&" in ln and "act[sel] !=" in ln:
            ro_dispatch_line = ln
            break
    check(ro_dispatch_line is not None,
          "app_mon_menu_readonly: no dispatch-whitelist line (`ro_native && act[sel] != ...`) found "
          "-- review F4 replaced the blunt early return with a whitelist; a bare "
          "`if (!empty && bc_is_native(rec)) { snd_deny(); return false; }` would fail this")
    if ro_dispatch_line is not None:
        for action in ("RO_VIEW", "RO_CANCEL"):
            check(action in ro_dispatch_line, f"app_mon_menu_readonly: dispatch-whitelist line does not name {action}")
        for action in ("RO_ITEM", "RO_MOVE", "RO_RELEASE", "RO_LEGAL", "RO_COPY", "RO_PASTE",
                        "RO_CREATE", "RO_DUP", "RO_DAYCARE", "RO_EXPORT"):
            check(action not in ro_dispatch_line,
                  f"app_mon_menu_readonly: dispatch-whitelist line unexpectedly names {action}")

    # ---- (e) exact count of xg_native_escape_denied( in pdna_box.c ----
    EXPECTED_COUNT = 4   # drop_held, drop_chunk, the party site, the homeless B-cancel
                         # (review F1/F2 use bc_is_native() directly -- unaffected)
    actual_count = len(GATE_RE.findall(box_text_stripped))
    check(actual_count == EXPECTED_COUNT,
          f"pdna_box.c: expected exactly {EXPECTED_COUNT} xg_native_escape_denied( call sites, found {actual_count}")

    # ---- (g) BACKLOG #150 S150-4: both start_carry() sites reachable with a
    # BOXSCOPE_GB source are guarded by src_can_lift(src, box, cur) -- the second
    # route the s150-4-5 brief missed (STOP-LICENCE, resolved by the orchestrator
    # 2026-09-15: same guard as the first site, nothing bigger). Pinned COUNT, not
    # presence alone (review F3's own lesson: presence-only checks can be masked by
    # an unrelated call elsewhere in the same body) -- pdna_box's body must contain
    # `src_can_lift(src, box, cur)` exactly 4 times: the CM_MOVE cursor-cycle guard,
    # the CM_MOVE dispatch gate before begin_select(), the CM_ITEM held-item gate
    # (unrelated to start_carry but the same predicate/args, decision 8), and the
    # NORMAL-mode MOVE-menu site's combined `!src_can_lift(...) || !start_carry(...)`
    # guard.
    s, e = extract_function(box_lines, r"^int pdna_box\(BoxSource\* src\)")
    pdna_box_body = box_lines[s:e]
    lift_count = sum(1 for ln in pdna_box_body if SRC_CAN_LIFT_CUR_RE.search(ln))
    check(lift_count == 4,
          f"pdna_box(): expected exactly 4 src_can_lift(src, box, cur) sites "
          f"(cursor-cycle, begin_select's dispatch gate, the CM_ITEM gate, the "
          f"NORMAL-mode MOVE-menu guard), found {lift_count}")
    # the NORMAL-mode site's specific shape: start_carry consumed in the SAME `if`
    # condition as src_can_lift, so a refused lift can never fall through to the
    # grab animation.
    check(any(COMBINED_GUARD_RE.search(ln) for ln in pdna_box_body),
          "pdna_box(): the NORMAL-mode MOVE-menu site's combined "
          "`!src_can_lift(...) || !start_carry(...)` guard line not found")
    # begin_select's own single-tap grab: start_carry's return is consumed in the
    # SAME `if` as the occupancy check (`g_box[anchor].species && start_carry(...)`),
    # not called as a bare statement whose return is silently discarded.
    s2, e2 = extract_function(box_lines, r"^static uint8_t\* begin_select\(")
    begin_select_body = box_lines[s2:e2]
    BEGIN_SELECT_CONSUME_RE = re.compile(
        r"g_box\[anchor\]\.species && start_carry\(src, recs, box, anchor\)")
    check(any(BEGIN_SELECT_CONSUME_RE.search(ln) for ln in begin_select_body),
          "begin_select(): start_carry(...)'s return is not consumed alongside the "
          "occupancy check -- a refused GB lift could fall through unnoticed")

    # ---- (i) REVIEW F2: drop_held's UP branch commits the Bank write BEFORE it ever
    # calls release_up (the Game Boy delete) -- pins the order, not just presence. ----
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    ok, d = up_order_facts(box_lines, s, e)
    check(ok, d)

    # ---- (j) BACKLOG #171: every is_bank-guarded `s_tab_focus = 1;` site in pdna_box()'s
    # carrying block also carries `|| src->bank_edge`, and there is exactly ONE such
    # site -- pinned so a second/third/fourth site added later without the clause is a
    # forced, deliberate look, not a silent pass. ----
    s, e = extract_function(box_lines, r"^int pdna_box\(BoxSource\* src\)")
    pdna_box_body = box_lines[s:e]
    block = carrying_block(pdna_box_body)
    tab1_count = sum(1 for ln in block if TAB1_ASSIGN_RE.search(ln))
    check(tab1_count == 1,
          f"pdna_box()'s carrying block: expected exactly 1 `s_tab_focus = 1;` site, "
          f"found {tab1_count} -- BACKLOG #171's own site count drifted, look before "
          f"trusting the per-site check below")
    ok, details = bank_edge_sites_check(block)
    check(ok, "BACKLOG #171: " + "; ".join(d for d in details if "missing" in d))

    # ---- (l) BACKLOG #142: the entry-time (non-carrying) `st == 1` arrival site
    # never sends an is_bank source to the SAVE tab -- see ARRIVAL_ST1_RE's own
    # comment above for why is_bank at this site always means a Game Boy session. ----
    ok, sites = arrival_never_saves_check(pdna_box_body)
    check(ok, f"BACKLOG #142: the st==1 arrival site sends an is_bank source to the "
          f"SAVE tab (expected exactly 1 clean site, found {sites})")

    # ---- (k) BACKLOG #171/#171b review F1: begin_select's refusal branch sets
    # *pfull = true -- one of this repo's four named partial-repaint trap classes: a
    # full-screen paint from OUTSIDE pdna_box()'s own render pipeline (gb_pick_origin's
    # ui_clear/ui_text picker, opened from inside lift_up()) leaves a stale bitmap on
    # screen until the NEXT need_full repaint; a bare `else snd_deny();` here left that
    # repaint to chance -- found live in mGBA (lane s150-4-5b): a REFUSED lift (the
    # origin prompt drawn, then cancelled at the serial step) left the picker's text on
    # screen with box icons drawn over it, un-erased, until an UNRELATED L/R box-switch
    # forced a real repaint. ----
    check(any(BEGIN_SELECT_REFUSAL_RE.search(ln) for ln in begin_select_body),
          "begin_select(): the refusal branch does not set *pfull = true -- a REFUSED "
          "lift (e.g. gb_pick_origin's full-screen picker, cancelled at the serial "
          "step) leaves a stale full-screen paint on screen with no forced repaint")
    # ---- REVIEW F2 (BACKLOG #150 S150-7): bank_down_exact's own DOWN-side order --
    # accept_down() (the persist) must come before app_bank_clear_slots() (the Bank
    # consume), the D7/D12 ordering that makes a refused/failed consume a duplicate
    # rather than a loss. ----
    sd, ed = extract_function(box_lines, r"^static BankDownResult bank_down_exact\(")
    ok, d = down_order_facts(box_lines, sd, ed)
    check(ok, d)

    # ---- (f) review F3: the self-mutation harness, every run ----
    self_test_mutation_detection(box_lines)

    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


def self_test_mutation_detection(box_lines: list[str]) -> None:
    """Review F3: prove the checker actually has teeth, on every run, not just when a
    human remembers to demonstrate it by hand. Builds two synthetic mutated copies of
    the real (comment-stripped) pdna_box.c body text and asserts gate_before_pattern()
    reports FAILURE on each -- if either mutation goes undetected, that is itself a
    failed check in this test's own tally."""
    # MUT A: delete begin_select's real guard (the two-line
    # `for (...) if (bc_is_native(...)) { snd_deny(); return recs; }` block right before
    # the copy-into-chunk loop).
    s, e = extract_function(box_lines, r"^static uint8_t\* begin_select\(")
    body = box_lines[s:e]
    # the real guard is a 2-line `for` + `if (bc_is_native(...)) { snd_deny(); return
    # recs; }`; drop every line naming bc_is_native within this function so the pattern
    # truly disappears, the same end state as deleting the guard block outright.
    mut_a = [ln for ln in body if not BC_NATIVE_RE.search(ln)]
    ok, detail = gate_before_pattern(mut_a, 0, len(mut_a), BC_NATIVE_RE, CHUNK_COPY_RE, "begin_select (MUT A)")
    check(not ok, f"MUT A (begin_select's guard deleted) should have been caught but was not: {detail}")
    print(f"  MUT A demonstration -- guard call removed from begin_select: {detail}")

    # MUT F: move drop_held's whole gate block (the `if (xg_native_escape_denied(...)) {...}`
    # statement, decision 3) to AFTER all five memcpy sites -- i.e. simulate the gate
    # existing but LOSING its dominating position.
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    body = box_lines[s:e]
    gate_start = None
    for i, ln in enumerate(body):
        if GATE_RE.search(ln):
            gate_start = i
            break
    if gate_start is None:
        check(False, "drop_held's own gate line vanished from the real source -- fix the source, not this test")
        return
    # the gate is a single `if (xg_native_escape_denied(s_held, src->scope)) {` line
    # followed by its brace-balanced block; find its close by brace counting from gate_start.
    depth = 0
    gate_end = gate_start
    for i in range(gate_start, len(body)):
        depth += body[i].count("{") - body[i].count("}")
        if depth <= 0 and i > gate_start:
            gate_end = i
            break
    gate_block = body[gate_start:gate_end + 1]
    rest = body[:gate_start] + body[gate_end + 1:]
    last_memcpy = None
    for i, ln in enumerate(rest):
        if MEMCPY80_RE.search(ln):
            last_memcpy = i
    if last_memcpy is None:
        check(False, "drop_held has no 80-byte memcpy left after removing the gate block -- unexpected shape")
        return
    mut_f = rest[:last_memcpy + 1] + gate_block + rest[last_memcpy + 1:]
    ok, detail = gate_before_pattern(mut_f, 0, len(mut_f), GATE_RE, MEMCPY80_RE, "drop_held (MUT F)")
    check(not ok, f"MUT F (drop_held's gate moved after all memcpys) should have been caught but was not: {detail}")
    print(f"  MUT F demonstration -- gate block relocated after drop_held's last memcpy: {detail}")

    # MUT G (BACKLOG #150 S150-4): revert the NORMAL-mode MOVE-menu site's combined
    # guard back to what it looked like before this lane closed the second G-M7
    # route -- an unconditional `start_carry(src, recs, box, cur);` with no
    # src_can_lift() check at all. The count check (g) must drop from 4 to 3 and fail.
    s, e = extract_function(box_lines, r"^int pdna_box\(BoxSource\* src\)")
    body = box_lines[s:e]
    combined_re = re.compile(
        r"if\s*\(\s*!src_can_lift\(src, box, cur\)\s*\|\|\s*!start_carry\(src, recs, box, cur\)\s*\)\s*\{")
    mut_g = []
    replaced = False
    i = 0
    while i < len(body):
        ln = body[i]
        if not replaced and combined_re.search(ln):
            # Replace the whole `if (...) { snd_deny(); } else { ... }` shape with a
            # single unconditional `start_carry(src, recs, box, cur);` line -- brace-
            # balanced removal of the if/else block this line opens.
            depth = 0
            j = i
            depth += ln.count("{") - ln.count("}")
            j += 1
            while depth > 0 and j < len(body):
                depth += body[j].count("{") - body[j].count("}")
                j += 1
            # j now points just past the matching close of the `if` block; consume a
            # following `else { ... }` too, if present, the same way.
            k = j
            while k < len(body) and body[k].strip() == "":
                k += 1
            if k < len(body) and "else" in body[k]:
                depth2 = body[k].count("{") - body[k].count("}")
                m = k + 1
                while depth2 > 0 and m < len(body):
                    depth2 += body[m].count("{") - body[m].count("}")
                    m += 1
                j = m
            mut_g.append("          start_carry(src, recs, box, cur);")
            i = j
            replaced = True
            continue
        mut_g.append(ln)
        i += 1
    check(replaced, "MUT G: could not locate the NORMAL-mode combined guard block to revert -- fix this test")
    lift_count_mut = sum(1 for ln in mut_g if SRC_CAN_LIFT_CUR_RE.search(ln))
    check(lift_count_mut == 3,
          f"MUT G (NORMAL-mode guard reverted to unconditional start_carry) should have "
          f"dropped the src_can_lift(src, box, cur) count to 3, got {lift_count_mut}")
    print(f"  MUT G demonstration -- NORMAL-mode MOVE-menu guard reverted to unconditional "
          f"start_carry: count dropped to {lift_count_mut} (expected 3, was 4)")

    # MUT H (REVIEW F2): swap the release_up() line to ABOVE the `ok = src->commit()`
    # line in a copy of drop_held's body -- the exact defect the reviewer demonstrated
    # (the Game Boy save would lose the mon before the Bank has committed it) -- and
    # assert up_order_facts() reports failure.
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    body = box_lines[s:e]
    commit_i = first_match_line(body, 0, len(body), COMMIT_RE)
    release_i = first_match_line(body, 0, len(body), RELEASE_UP_RE)
    check(commit_i is not None and release_i is not None,
          "MUT H: could not locate both the commit() and release_up() lines in the real source -- fix this test")
    if commit_i is not None and release_i is not None and commit_i < release_i:
        mut_h = list(body)
        release_line = mut_h.pop(release_i)
        mut_h.insert(commit_i, release_line)   # release_up's line now sits BEFORE commit()
        ok, detail = up_order_facts(mut_h, 0, len(mut_h))
        check(not ok, f"MUT H (release_up swapped before commit()) should have been caught but was not: {detail}")
        print(f"  MUT H demonstration -- release_up() line swapped above src->commit(): {detail}")

    # MUT I (BACKLOG #171): revert the ONE real, clause-carrying `s_tab_focus = 1;`
    # site back to its pre-fix `else if (!src->is_bank) { s_tab_focus = 1; ... }` form
    # (no `|| src->bank_edge`) -- the exact dead-carry regression lane s150-4-5b found
    # live in mGBA -- and assert bank_edge_sites_check() catches it.
    s, e = extract_function(box_lines, r"^int pdna_box\(BoxSource\* src\)")
    pdna_box_body = box_lines[s:e]
    block = carrying_block(pdna_box_body)
    mut_i = list(block)
    reverted = False
    for i, ln in enumerate(mut_i):
        if TAB1_ASSIGN_RE.search(ln) and BANK_EDGE_CLAUSE_RE.search(ln):
            mut_i[i] = ln.replace(" || src->bank_edge", "")
            reverted = True
            break
    check(reverted, "MUT I: could not find the real, clause-carrying `s_tab_focus = 1;` "
                     "site to revert -- fix this test")
    if reverted:
        ok, details = bank_edge_sites_check(mut_i)
        check(not ok, f"MUT I (bank_edge clause stripped) should have been caught but "
                       f"was not: {details}")
        print(f"  MUT I demonstration -- `|| src->bank_edge` stripped from the "
              f"s_tab_focus=1 site: {details}")

    # MUT J (BACKLOG #171/#171b review F1): revert begin_select's refusal branch back
    # to its pre-fix bare `else snd_deny();` form (no *pfull = true) -- the exact
    # stale-full-screen-paint regression lane s150-4-5b found live in mGBA (the
    # refused-origin-prompt frame left un-erased with box icons drawn over it) -- and
    # assert the checker catches it.
    s, e = extract_function(box_lines, r"^static uint8_t\* begin_select\(")
    body = box_lines[s:e]
    mut_j = list(body)
    reverted_j = False
    for i, ln in enumerate(mut_j):
        if BEGIN_SELECT_REFUSAL_RE.search(ln):
            mut_j[i] = BEGIN_SELECT_REFUSAL_RE.sub("else snd_deny();", ln)
            reverted_j = True
            break
    check(reverted_j, "MUT J: could not find the real `*pfull = true` refusal branch "
                       "to revert -- fix this test")
    if reverted_j:
        ok = any(BEGIN_SELECT_REFUSAL_RE.search(ln) for ln in mut_j)
        check(not ok, "MUT J (refusal branch reverted to bare `else snd_deny();`) "
                       "should have been caught but was not")
        print("  MUT J demonstration -- begin_select's refusal branch reverted to "
              "bare `else snd_deny();` (no *pfull = true): correctly caught")
    # MUT L (REVIEW F2): swap accept_down() and app_bank_clear_slots() in a copy of
    # bank_down_exact's body -- the exact inversion the reviewer demonstrated -- and
    # assert down_order_facts() reports failure.
    sd, ed = extract_function(box_lines, r"^static BankDownResult bank_down_exact\(")
    down_body = box_lines[sd:ed]
    accept_i = first_match_line(down_body, 0, len(down_body), ACCEPT_DOWN_RE)
    consume_i = first_match_line(down_body, 0, len(down_body), CONSUME_RE)
    check(accept_i is not None and consume_i is not None,
          "MUT L: could not locate both the accept_down() and app_bank_clear_slots() "
          "lines in the real source -- fix this test")
    if accept_i is not None and consume_i is not None and accept_i < consume_i:
        mut_i = list(down_body)
        consume_line = mut_i.pop(consume_i)
        mut_i.insert(accept_i, consume_line)   # the Bank consume's line now sits BEFORE accept_down()
        ok, detail = down_order_facts(mut_i, 0, len(mut_i))
        check(not ok, f"MUT L (app_bank_clear_slots swapped before accept_down()) should have been "
              f"caught but was not: {detail}")
        print(f"  MUT L demonstration -- app_bank_clear_slots() line swapped above "
              f"accept_down(): {detail}")
    # MUT M (BACKLOG #150 S150-8 review F5): `bool converted = true;` at drop_held's own
    # top -- the exact defect the reviewer demonstrated (passes check (a) above, which
    # only looks at line POSITION relative to the first 80-byte memcpy, never the gate's
    # own condition) while unconditionally bypassing the native-escape gate for every
    # drop. The new per-site checks above must catch it.
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    body = box_lines[s:e]
    decl_i = first_match_line(body, 0, len(body), re.compile(r"bool converted = false;"))
    check(decl_i is not None, "MUT M: could not locate `bool converted = false;` in the real source -- fix this test")
    if decl_i is not None:
        mut_i = list(body)
        mut_i[decl_i] = mut_i[decl_i].replace("converted = false", "converted = true")
        gate_idx = [i for i in range(len(mut_i)) if GATE_RE.search(mut_i[i])]
        gate_ok = len(gate_idx) == 1 and "!converted &&" in mut_i[gate_idx[0]]
        # the gate TEXT is unaffected by this mutation (still reads `!converted &&`), so
        # what must actually catch MUT M is the SECOND check: `converted = true` now
        # appears twice (the declaration-turned-assignment plus the real, legitimate
        # one after BANK_DOWN_CONVERTED) -- exactly the ambiguity a reviewer would flag.
        asg_idx = [i for i, ln in enumerate(mut_i) if CONVERTED_TRUE_RE.search(ln)]
        asg_ok = len(asg_idx) == 1 and any(
            "BANK_DOWN_CONVERTED" in mut_i[j] for j in range(max(0, asg_idx[0] - 3), asg_idx[0]))
        check(gate_ok, "MUT M: unexpectedly broke the gate-text check too -- fix this test's mutation")
        check(not asg_ok, "MUT M (`converted` initialised true) should have been caught by the "
                          "single-assignment check but was not")
        print(f"  MUT M demonstration -- `bool converted = true;` at drop_held's own top: "
              f"{len(asg_idx)} `converted = true` occurrence(s) found (expected exactly 1, "
              f"immediately after a BANK_DOWN_CONVERTED test)")

    # MUT N (BACKLOG #142, lane b142): revert the entry-time `st == 1 && !s_holding`
    # arrival site back to its pre-fix `s_tab_focus = src->is_bank ? 2 : 1;` form --
    # the exact regression this lane fixed (a Game Boy session's own box grid parked
    # the cursor on the SAVE tab on a directional arrival from its linked Bank) -- and
    # assert arrival_never_saves_check() catches it.
    s, e = extract_function(box_lines, r"^int pdna_box\(BoxSource\* src\)")
    pdna_box_body = box_lines[s:e]
    mut_n = list(pdna_box_body)
    reverted_n = False
    for i, ln in enumerate(mut_n):
        if ARRIVAL_ST1_RE.search(ln) and "!src->is_bank" in ln:
            mut_n[i] = ln.replace("st == 1 && !s_holding && !src->is_bank) s_tab_focus = 1;",
                                   "st == 1 && !s_holding) s_tab_focus = src->is_bank ? 2 : 1;")
            reverted_n = True
            break
    check(reverted_n, "MUT N: could not find the real fixed arrival site to revert -- "
                       "fix this test")
    if reverted_n:
        ok, sites = arrival_never_saves_check(mut_n)
        check(not ok, f"MUT N (arrival site reverted to `is_bank ? 2 : 1`) should have "
                       f"been caught but was not: {sites}")
        print(f"  MUT N demonstration -- arrival site reverted to the pre-fix "
              f"`is_bank ? 2 : 1` ternary: correctly caught ({sites})")


if __name__ == "__main__":
    sys.exit(main())
