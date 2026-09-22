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
  (n) merged-tree review F4: bank_down_dispatch's (source/pdna_box.c) three case labels
      each bind to their OWN callee -- XG_DOWN_ARM_EXACT -> bank_down_exact(,
      XG_DOWN_ARM_GB_BRIDGE -> bank_down_convert_gb(, XG_DOWN_ARM_GEN3 ->
      bank_down_convert_gen3( -- on the same line or the next non-blank line. Before
      this, only the switch's EXISTENCE was pinned, never WHICH callee each case
      binds to, so a callee swap between two cases survived every other check in this
      file. MUT O swaps the GB_BRIDGE and GEN3 callees and must be caught.
  (o) merged-tree review F4: drop_held's non-EXACT LANDED tail (the block after
      `if (arm == XG_DOWN_ARM_EXACT) { ... return recs; }`, still inside
      `if (bd == BANK_DOWN_LANDED)`) must contain app_bank_clear_slots( (the immediate
      Bank consume, 59dd45c) and must NOT contain app_bank_defer_delete( (which would
      wait for a Gen-3 exit-save flush a Game Boy session never runs, merged-tree
      review F2). MUT P deletes the app_bank_clear_slots( call and MUT Q replaces it
      with app_bank_defer_delete(; both must be caught.
  (p) merged-tree review F4/F1: gb_bank_down_bridge (source/pdna_gen12.c) computes its
      destination generation as `uint8_t dst_gen = g_ed->s.gen;` (the MOUNTED
      session), never the inverted `? GB_GEN2 : GB_GEN1` form review F1 removed. MUT R
      restores the inverted line and must be caught.
  (q) merged-tree reviewer: the same non-EXACT LANDED tail must ALSO repaint --
      `s_oam_reload = true;` AND a `recs = src->records(box)` reassignment -- so a
      revert that keeps the Bank consume but drops the repaint (leaving the on-screen
      box stale after a GB_BRIDGE drop) is caught. MUT W drops both lines.
  (r) merged-tree reviewer, negative form of review F2: the tail must contain NO
      app_bank_defer_delete(, even alongside a KEPT app_bank_clear_slots( -- the
      regression is re-adding the defer, not just replacing the immediate consume
      (that shape is MUT Q). MUT T re-adds the defer call without removing the consume.
  (s) merged-tree reviewer, MUT L's shape: the tail's app_bank_clear_slots( call must
      come AFTER the bank_down_dispatch( call in text order. MUT U moves it before.
  (t) merged-tree reviewer: drop_held derives `arm` via xg_bank_down_arm( exactly
      ONCE, above the bank_down_dispatch( call -- the union's original bug class was a
      second derivation in the tail. MUT V adds a second derivation after dispatch.
  (aa) BACKLOG #150 S150-10: gb_paste_hook (source/pdna_gen12.c) computes bad4 via
      gb_clip_moves( (which wraps g3gb_moves_ok) BEFORE its first gen3_to_gb_fixed(
      call. MUT X deletes the gb_clip_moves( call and must be caught. (Re-lettered
      from (u) at the merge with main/s150-12, which already owns (u)/(v)/(w); s150-9
      is taking (x)/(y)/(z), so (aa)/(ab)/(ac) are the next free ones.)
  (ab) BACKLOG #150 S150-10 decision 8.7: gb_paste_hook contains the zero-move refusal
      (`nbad == 4 && nfill == 0`) -- the ONE exception to Guy's "never block" answer,
      for a mon that would otherwise land with no moves at all (Struggles forever, an
      illegal Game Boy record). MUT Y weakens the condition to `false` and must be
      caught. (Re-lettered from (v), same merge.)
  (ac) BACKLOG #150 S150-10: gb_paste_hook calls gb_paste_fill_moves( (the fill) BEFORE
      gb_paste_write( -- a bad slot that reached gen3_to_gb_fixed with a non-NULL bad4
      must always be filled (or the whole paste refused by (ab)) before the record is
      ever written. MUT Z moves the fill call after the write and must be caught.
      (Re-lettered from (w), same merge.)
  (ad) BACKLOG #168a review D2: drop_held_up's ident32-collision scan runs once per
      session regardless of pdna_bank_serial_trusted() -- the `s_up_scan_done` latch
      must be part of the same guard line, not a bare `!pdna_bank_serial_trusted()`
      check (main's own check, not a #212 one -- documented here since #212's
      re-verify R4 freed this letter from #210's busy-reading check below, which
      re-lettered to (ag)). MUT AD reverts the guard to the bare form and must be
      caught.
  (ag) BACKLOG #210: gb_paste_hook calls s_busy_reading( BEFORE gb_paste_fill_moves( --
      the fill's own gb_create_locate_rom() call is CREATE's identical cold, uncached,
      ~185,000-read full-ROM scan (see that function's own MEDIUM-1 comment), and
      CREATE masks the same scan with s_busy_reading() before it runs. A busy call
      missing, or sitting after the fill call, would leave a cold paste's loss screen
      -> modal transition looking frozen for the whole scan. MUT AG deletes the
      s_busy_reading( line and must be caught. (Re-lettered from (ad) at #212's
      re-verify R4, MUT AA -> MUT AG.)
  (aj) BACKLOG #212: gb_bank_down_bridge (source/pdna_gen12.c) calls s_busy_reading(
      BEFORE gb_paste_fill_moves( too -- the bridge shares gb_paste_fill_moves' own
      cold ROM scan with gb_paste_hook (ag, above). MUT AJ deletes the s_busy_reading(
      call and must be caught.
  (ah) BACKLOG #212: gb_bank_down_bridge calls gb_paste_fill_moves( (the fill, S150-10's
      own function reused for the bridge) BEFORE gb_paste_legal_screen_ex( (the modal)
      -- the fills must already be in `mon` when the swap-row modal lists them, exactly
      gb_paste_hook's own step 7/8 order. MUT AH moves the fill call after the modal and
      must be caught. (Re-lettered from (ae) at R4, MUT AB -> MUT AH.)
  (ai) BACKLOG #212: gb_bank_down_bridge calls gb_paste_legal_screen_ex( (the modal)
      BEFORE gbs_insert( (the box write) -- CANCEL must be able to discard `mon`
      before anything lands on the card. MUT AI moves the modal call after the write
      and must be caught. (Re-lettered from (af) at R4, MUT AC -> MUT AI.)
  (ak) BACKLOG #212 re-verify R3: gb_bank_down_bridge contains the SAME `nleft == 0`
      zero-move refusal (ab) gives gb_paste_hook -- D9's own guard, closing the same
      Struggle-forever hole for a bridge paste. Mirrors (ab); no MUT of its own here
      (reuses ZERO_MOVE_REFUSAL_RE and (ab)'s own MUT Y demonstration).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOX_C = ROOT / "source" / "pdna_box.c"
MAIN_C = ROOT / "source" / "pdna_main.c"
BANK_C = ROOT / "source" / "pdna_bank.c"
GEN12_C = ROOT / "source" / "pdna_gen12.c"
XFER_IO_C = ROOT / "source" / "xfer_io.c"

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

# BACKLOG #150 S150-8b: the RESTORE edge's ordering, drop_held's PC->Bank arm. Shared
# by the real check (l) and its self-mutation demonstration (MUT K).
RESTORE_UP_RE       = re.compile(r"\bpc_bank_restore_up\(")
RESTORE_DONE_RE     = re.compile(r"\bpc_bank_restore_done\(")
RESTORE_MEMCPY_RE   = re.compile(r"memcpy\(recs \+ \(uint32_t\)cur \* 80, rc == 1")
PC_RELEASE_SLOT_RE  = re.compile(r"\bapp_pc_release_slot\(")
APP_CAN_EDIT_RE     = re.compile(r"\bapp_can_edit\(")
SF_WRITE_VERIFIED_RE = re.compile(r"\bsf_write_verified\(")
BC_PACK_RE          = re.compile(r"\bbc_pack\(")
XR_OPEN_RE          = re.compile(r"\bxr_open\(")
XR_STATE_RESTORED_RE = re.compile(r"\be\.state = XR_STATE_RESTORED;")

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

# BACKLOG #150 S150-9 step 5: site 2's ordering (the GB lift restores a native-home
# entry through the merge screen; the release re-verifies via the ledger and marks it
# RESTORED). Checks (u)/(v)/(w) below -- the brief's own drafting-time letters (m)/
# (n)/(o) collide with checks already landed by intervening lanes in this file
# (2026-09-16's occupied-scope check claimed (m); the merged-tree review claimed (n)/
# (o)) -- re-lettered here, disclosed in the delivery report. MUT X/Y/Z are the next
# free mutation letters (A through W are all taken).
GB_HAS_SIDECAR_RE        = re.compile(r"\bgb_has_sidecar\(")
GB_LIFT_RESTORE_CALL_RE  = re.compile(r"\bgb_lift_restore\(")
APP_XFER_MERGE_SCREEN_RE = re.compile(r"\bapp_xfer_merge_screen\(")
BANK_NEXT_SERIAL_RE      = re.compile(r"\bpdna_bank_next_serial\(")
BANK_RESTORE_GB_RE       = re.compile(r"\bbank_restore_from_entry_gb\(")
GBS_DELETE_RE            = re.compile(r"\bgbs_delete\(")
GB_PERSIST_RE            = re.compile(r"\bgb_persist\(")
RELEASE_VERIFY_CALL_RE   = re.compile(r"\bgb_release_restored_verify\(")
RESTORE_UP_RESTORED_CHECK_RE = re.compile(r"e\.state == XR_STATE_RESTORED")
RESTORE_UP_PENDING_CHECK_RE  = re.compile(r"e\.state == XR_STATE_PENDING")


def lift_order_facts_hook(hook_body: list[str]) -> tuple[bool, str]:
    """Check (u), part 1: in gb_lift_up_hook's body, gb_has_sidecar( (the cheap f_stat
    pre-check) must be found and must precede the gb_lift_restore( call it gates."""
    hs = first_match_line(hook_body, 0, len(hook_body), GB_HAS_SIDECAR_RE)
    lr = first_match_line(hook_body, 0, len(hook_body), GB_LIFT_RESTORE_CALL_RE)
    if hs is None or lr is None:
        return False, "gb_lift_up_hook: could not find gb_has_sidecar(/gb_lift_restore( calls"
    if not (hs < lr):
        return False, (f"gb_lift_up_hook: gb_has_sidecar( (line {hs + 1}) does not come "
                        f"before gb_lift_restore( (line {lr + 1})")
    return True, "ok"


def lift_order_facts_restore(restore_body: list[str]) -> tuple[bool, str]:
    """Check (u), part 2: inside gb_lift_restore's own body -- the screen before the
    serial is spent, the serial before the pack (decision 10's own ordering)."""
    ace = first_match_line(restore_body, 0, len(restore_body), APP_CAN_EDIT_RE)
    xo  = first_match_line(restore_body, 0, len(restore_body), XR_OPEN_RE)
    scr = first_match_line(restore_body, 0, len(restore_body), APP_XFER_MERGE_SCREEN_RE)
    ser = first_match_line(restore_body, 0, len(restore_body), BANK_NEXT_SERIAL_RE)
    brg = first_match_line(restore_body, 0, len(restore_body), BANK_RESTORE_GB_RE)
    vals = {"app_can_edit(": ace, "xr_open(": xo, "app_xfer_merge_screen(": scr,
            "pdna_bank_next_serial(": ser, "bank_restore_from_entry_gb(": brg}
    missing = [n for n, v in vals.items() if v is None]
    if missing:
        return False, f"gb_lift_restore: missing call(s): {missing}"
    if not (ace < xo < scr < ser < brg):
        return False, f"gb_lift_restore: wrong order -- {vals} (want strictly increasing)"
    return True, "ok"


def release_order_facts(body: list[str]) -> tuple[bool, str]:
    """Check (v): gb_release_up_hook -- gb_release_restored_verify( before gbs_delete(,
    gbs_delete( before gb_persist(, and the RESTORED mark STRICTLY AFTER gb_persist(."""
    v = first_match_line(body, 0, len(body), RELEASE_VERIFY_CALL_RE)
    d = first_match_line(body, 0, len(body), GBS_DELETE_RE)
    p = first_match_line(body, 0, len(body), GB_PERSIST_RE)
    m = first_match_line(body, 0, len(body), XR_STATE_RESTORED_RE)
    vals = {"gb_release_restored_verify(": v, "gbs_delete(": d, "gb_persist(": p,
            "e.state = XR_STATE_RESTORED;": m}
    missing = [n for n, val in vals.items() if val is None]
    if missing:
        return False, f"gb_release_up_hook: missing call(s): {missing}"
    if not (v < d):
        return False, (f"gb_release_up_hook: gb_release_restored_verify( (line {v + 1}) "
                        f"does not come before gbs_delete( (line {d + 1})")
    if not (d < p):
        return False, (f"gb_release_up_hook: gbs_delete( (line {d + 1}) does not come "
                        f"before gb_persist( (line {p + 1})")
    if not (p < m):
        return False, (f"gb_release_up_hook: the RESTORED mark (line {m + 1}) does not come "
                        f"STRICTLY AFTER gb_persist( (line {p + 1})")
    return True, "ok"


def restore_up_state_before_screen_facts(body: list[str]) -> tuple[bool, str]:
    """Check (w): pc_bank_restore_up -- both state refusals (RESTORED/PENDING) precede
    app_xfer_merge_screen(, which itself precedes pdna_bank_next_serial( (the state
    refusals spend nothing; the screen precedes the serial)."""
    r = first_match_line(body, 0, len(body), RESTORE_UP_RESTORED_CHECK_RE)
    p = first_match_line(body, 0, len(body), RESTORE_UP_PENDING_CHECK_RE)
    scr = first_match_line(body, 0, len(body), APP_XFER_MERGE_SCREEN_RE)
    ser = first_match_line(body, 0, len(body), BANK_NEXT_SERIAL_RE)
    vals = {"e.state == XR_STATE_RESTORED": r, "e.state == XR_STATE_PENDING": p,
            "app_xfer_merge_screen(": scr, "pdna_bank_next_serial(": ser}
    missing = [n for n, v in vals.items() if v is None]
    if missing:
        return False, f"pc_bank_restore_up: missing: {missing}"
    if not (r < scr and p < scr):
        return False, (f"pc_bank_restore_up: a state refusal does not precede "
                        f"app_xfer_merge_screen( -- {vals}")
    if not (scr < ser):
        return False, (f"pc_bank_restore_up: app_xfer_merge_screen( (line {scr + 1}) does "
                        f"not come before pdna_bank_next_serial( (line {ser + 1})")
    return True, "ok"


def arrival_never_saves_check(pdna_box_body: list[str]) -> tuple[bool, list[str]]:
    """BACKLOG #142: exactly one `st == 1 && !s_holding` site must exist in
    pdna_box()'s own body, and none of its matching lines may carry the
    `is_bank ? 2 : 1` ternary that sends an is_bank arrival to the SAVE tab.
    Shared by the real check (l) and its self-mutation demonstration (MUT S)."""
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

# BACKLOG #150 S150-12 decision 6: the release_up-optional wrap in drop_held's UP
# branch. Shared by the real check (n1) and its self-mutation demonstration (MUT N1).
RELEASE_UP_TEST_RE = re.compile(r"if\s*\(\s*s_xfer_peer->release_up\s*\)")   # NOT a call (no `(` after
                                                                              # the member) -- must not
                                                                              # match RELEASE_UP_RE
HOLDING_DONE_RE     = re.compile(r"s_holding = false; \*done = true;")
PC_QUEUE_NOTE_RE    = re.compile(r"app_pc_queue_note\(")


def n1_order_facts(lines, start, end):           # shared by the real check AND MUT N1
    """BACKLOG #150 S150-12 decision 6: `if (s_xfer_peer->release_up)` must exist,
    come AFTER both `ok = src->commit()` and the `s_holding = false; *done = true;`
    hand-empty line, and `app_pc_queue_note(` must sit in its else branch -- line
    order test < release_up call < app_pc_queue_note(."""
    c = first_match_line(lines, start, end, COMMIT_RE)
    h = first_match_line(lines, start, end, HOLDING_DONE_RE)
    t = first_match_line(lines, start, end, RELEASE_UP_TEST_RE)
    r = first_match_line(lines, start, end, RELEASE_UP_RE)
    q = first_match_line(lines, start, end, PC_QUEUE_NOTE_RE)
    if c is None: return False, "drop_held: no `ok = src->commit()` line in the UP branch"
    if h is None: return False, "drop_held: no `s_holding = false; *done = true;` line in the UP branch"
    if t is None: return False, "drop_held: no `if (s_xfer_peer->release_up)` test line -- decision 6's wrap is missing"
    if r is None: return False, "drop_held: no `s_xfer_peer->release_up(` call"
    if q is None: return False, "drop_held: no `app_pc_queue_note(` call"
    if not (c < t and h < t):
        return False, (f"drop_held: the `if (s_xfer_peer->release_up)` test (line {t+1}) does not come "
                        f"after both commit() (line {c+1}) and the hand-empty line (line {h+1})")
    if not (t < r < q):
        return False, (f"drop_held: expected line order test (line {t+1}) < release_up() call "
                        f"(line {r+1}) < app_pc_queue_note() (line {q+1})")
    return True, "ok"


def up_order_facts(lines, start, end):           # shared by the real check AND MUT H
    c = first_match_line(lines, start, end, COMMIT_RE)
    r = first_match_line(lines, start, end, RELEASE_UP_RE)
    z = first_match_line(lines, start, end, ZEROBACK_RE)
    if c is None: return False, "drop_held_up: no `ok = src->commit()` line in the UP branch"
    if r is None: return False, "drop_held_up: no `s_xfer_peer->release_up(` call"
    if z is None: return False, "drop_held_up: no zero-back memset of the destination cell"
    if not c < r: return False, (f"drop_held_up: src->commit() (line {c+1}) does NOT come before "
                                 f"release_up() (line {r+1}) -- the Game Boy save would lose the "
                                 f"mon before the Bank has it")
    if not c < z < r: return False, (f"drop_held_up: the zero-back memset (line {z+1}) is not between "
                                     f"commit() ({c+1}) and release_up() ({r+1})")
    if first_match_line(lines, z, r, RETURN_RECS_RE) is None:
        return False, (f"drop_held_up: no `return recs;` between the zero-back memset (line {z+1}) and "
                       f"release_up() (line {r+1}) -- release_up is not dominated by the "
                       f"commit-failure early-out")
    return True, "ok"


# BACKLOG #168a review D2 (check (ad), shared by the real check and MUT AD below):
# `pdna_bank_serial_trusted()` says only that the LAST meta_load() this session parsed
# a clean primary -- it cannot see a bank.meta restored from .bak, an older
# /PokeDNA/bank copied back, or a second card's boxes dropped in beside this card's
# meta. The fix pays the full 16-box scan ONCE per session regardless of `trusted`,
# via a `!s_up_scan_done` latch OR'd onto the same guard line the reviewer's original
# bare `if (!pdna_bank_serial_trusted())` used -- a revert to the bare form silently
# skips the WHOLE scan on an ordinary card (trusted stays true all session) even
# though it has never actually run once.
UP_SCAN_GUARD_RE = re.compile(r"if\s*\(\s*!pdna_bank_serial_trusted\(\)")
UP_SCAN_LATCH_RE = re.compile(r"s_up_scan_done")


def up_scan_latch_facts(lines, start, end):      # shared by the real check AND MUT AD
    g = first_match_line(lines, start, end, UP_SCAN_GUARD_RE)
    if g is None:
        return False, "drop_held_up: no `if (!pdna_bank_serial_trusted()` guard line found"
    if not UP_SCAN_LATCH_RE.search(lines[g]):
        return False, (f"drop_held_up: the scan guard (line {g+1}) does not also check "
                       f"`s_up_scan_done` -- on an ordinary card `trusted` stays true all "
                       f"session and the 16-box scan would never run even once")
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


# BACKLOG #150 S150-12 decision 9: a COPY cell's DOWN writes no ledger entry -- the
# `copy` derivation must precede the first ledger write, and every ledger-mutating
# call in gb_bank_down_gen3/gb_bank_down_bridge must sit inside an `if (!copy`
# guard (either form: a one-line `if (!copy) call(...);` or a brace-balanced
# `if (!copy) { ... call(...); ... }` block). Shared by the real check (n3) and its
# self-mutation demonstration (MUT N3).
COPY_DERIVE_RE       = re.compile(r"const bool copy = xg_cell_is_copy\(")
COPY_GUARD_RE        = re.compile(r"if\s*\(\s*!copy\b")
XFER_DOWN_WRITE_RE   = re.compile(r"\bxfer_down_write\(")
XFER_PENDING_SET_RE  = re.compile(r"\bapp_xfer_pending_set\(")
XFER_CLAIM_NOW_RE    = re.compile(r"\bxfer_down_claim_now\(")
XFER_UNDO_RE         = re.compile(r"\bxfer_down_undo\(")
N3_TARGET_RES = (XFER_DOWN_WRITE_RE, XFER_PENDING_SET_RE, XFER_CLAIM_NOW_RE, XFER_UNDO_RE)


def n3_facts_over_body(body, func_name):
    """The core of n3_facts(), operating on an already-extracted (and possibly
    mutated) function body list -- shared by the real check (n3) and MUT N3, same
    "real check and its self-mutation demonstration call the identical logic"
    posture as gate_before_pattern(). Approximates "is this call inside an
    `if (!copy` guard" with a brace-depth walk: a same-line guard (`if (!copy)
    call(...);`, no `{`) covers only its own line; a block guard (`if (!copy) {`)
    covers every line from the NEXT line until depth returns to the depth measured
    just before that guard line. This is a heuristic, not a real block-scope parser
    (documented per the brief) -- as precise as gate_before_pattern()'s own
    line-order approximation elsewhere in this file, applied to "inside a guard"
    instead of "before a line". Returns (ok, detail)."""
    copy_i = first_match_line(body, 0, len(body), COPY_DERIVE_RE)
    if copy_i is None:
        return False, f"{func_name}: no `const bool copy = xg_cell_is_copy(` derivation found"
    write_i = first_match_line(body, 0, len(body), XFER_DOWN_WRITE_RE)
    if write_i is not None and not copy_i < write_i:
        return False, (f"{func_name}: the `copy` derivation (line {copy_i+1}) does not precede "
                        f"the first xfer_down_write( (line {write_i+1})")

    depth = 0
    guard_stack = []   # depths at which an `if (!copy) {` block's own body sits
    violations = []
    for i, ln in enumerate(body):
        depth_before = depth
        has_guard = bool(COPY_GUARD_RE.search(ln))
        same_line_guard = has_guard and "{" not in ln
        opens_block = has_guard and "{" in ln
        guarded = same_line_guard or bool(guard_stack)
        if any(p.search(ln) for p in N3_TARGET_RES) and not guarded:
            violations.append(f"{func_name} line {i+1}: {ln.strip()!r} is not inside an `if (!copy` guard")
        if opens_block:
            guard_stack.append(depth_before)
        depth += ln.count("{") - ln.count("}")
        while guard_stack and depth <= guard_stack[-1]:
            guard_stack.pop()
    if violations:
        return False, "; ".join(violations)
    return True, "ok"


def n3_facts(lines, func_name):
    s, e = extract_function(lines, r"^BankDownResult " + re.escape(func_name) + r"\(")
    body = lines[s:e]
    return n3_facts_over_body(body, func_name)


# ---- REVIEW FIX (LOW, BACKLOG #150 S150-12): three findings the reviewer's own
# mutants M2/M7/M9 passed EVERY test in this file (unpinned, not a source defect --
# the shipped source is correct today, only unproven). Shared by the real checks (u)/
# (v)/(w) below and their own self-mutation demonstrations. -----------------------
K_GB_XFER_RO_RE = re.compile(r"static const BoxXferOps k_gb_xfer_ro = \{")


def k_gb_xfer_ro_facts(lines):     # shared by real check (u) and MUT M2
    """BACKLOG #150 S150-12 decision 2: k_gb_xfer_ro's literal must contain BOTH
    `.release_up = 0` and `.accept_down = 0` -- the read-only mount's table has
    STRUCTURALLY no delete/accept hook, not just a runtime-refused one (reviewer's
    mutant M2: a non-NULL release_up here would let a COPY drop delete from the
    read-only mount's own save, which has no GbSession to write through -- a
    guaranteed crash or silent corruption, not caught by any other check in this
    file, since drop_held's own `s_xfer_peer->release_up` guard only checks
    non-NULLness, never WHAT the pointer is)."""
    idx = first_match_line(lines, 0, len(lines), K_GB_XFER_RO_RE)
    if idx is None:
        return False, "k_gb_xfer_ro: `static const BoxXferOps k_gb_xfer_ro = {` literal not found"
    depth = 0
    end = idx
    for i in range(idx, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > idx:
            end = i
            break
    block = "\n".join(lines[idx:end + 1])
    missing = [f for f in (".release_up = 0", ".accept_down = 0") if f not in block]
    if missing:
        return False, f"k_gb_xfer_ro: literal is missing {missing} -- see this check's own docstring"
    return True, "ok"


HAVE_XFER_RE = re.compile(r"bool have_xfer = s_xfer_peer")


def have_xfer_facts(lines):        # shared by real check (v) and MUT M7
    """BACKLOG #150 S150-12 decision 6: the `have_xfer` line in drop_held's UP branch
    must NOT mention `release_up` -- requiring it would refuse the read-only mount's
    COPY drop outright (its table has no release_up at all), reverting decision 6's
    whole point. Exactly one `bool have_xfer = s_xfer_peer` site is expected
    (reviewer's mutant M7: adding `&& s_xfer_peer->release_up` back)."""
    idx = first_match_line(lines, 0, len(lines), HAVE_XFER_RE)
    if idx is None:
        return False, "drop_held: no `bool have_xfer = s_xfer_peer` line found"
    if "release_up" in lines[idx]:
        return False, (f"drop_held line {idx+1}: {lines[idx].strip()!r} mentions `release_up` -- "
                        f"this would refuse the read-only mount's COPY drop (its table has no "
                        f"release_up), reverting decision 6")
    return True, "ok"


NF_MASK_RE = re.compile(r"uint8_t nf = \(uint8_t\)\(meta\.flags & \(")


def native_summary_mask_facts(lines):   # shared by real check (w) and MUT M9
    """BACKLOG #150 S150-12 decision 3: gb_native_summary_open's `nf` re-pack mask
    (source/pdna_gen12.c) must carry BC_FLAG_COPY forward, or editing a copy cell
    through the summary screen silently un-marks it -- the NEXT DOWN of that cell
    then writes the clone's ledger entry, reaching G-L3 (the original clone-claims-
    the-original hole this whole lane exists to close) through the editor instead
    of through a fresh lift (reviewer's mutant M9: dropping `| BC_FLAG_COPY` from
    the mask)."""
    idx = first_match_line(lines, 0, len(lines), NF_MASK_RE)
    if idx is None:
        return False, "gb_native_summary_open: no `uint8_t nf = (uint8_t)(meta.flags & (...` re-pack line found"
    # the mask spans this line and (today) one continuation line before the closing `));`
    window = "\n".join(lines[idx:idx + 3])
    if "BC_FLAG_COPY" not in window:
        return False, ("gb_native_summary_open: the `nf` re-pack mask does not carry BC_FLAG_COPY "
                        "forward -- editing a copy cell would silently un-mark it (G-L3 through "
                        "the editor)")
    return True, "ok"


def restore_order_facts(lines, start, end):     # shared by the real check (l) AND MUT K
    """BACKLOG #150 S150-8b decision 3 / §3.2's commit order: drop_held's PC->Bank arm
    must call pc_bank_restore_up( BEFORE the ternary memcpy that writes either the
    rebuilt native cell or the plain Gen-3 record, and pc_bank_restore_done( (the
    entry-marking call) must come STRICTLY AFTER BOTH `ok = src->commit()` (the
    verified Bank write) and `app_pc_release_slot(` (the source release) -- the
    "destination first, verified; source only after; landed writes never undone" rule,
    applied to marking the ledger entry: it must never be visible as done before the
    Bank write it describes has actually landed."""
    u = first_match_line(lines, start, end, RESTORE_UP_RE)
    m = first_match_line(lines, start, end, RESTORE_MEMCPY_RE)
    # review D4: COMMIT_RE's FIRST match in drop_held's whole body is the UP path's
    # OWN `ok = src->commit()` (the earlier, unrelated Bank<-GB arm) -- anchoring the
    # search there let the PC->Bank arm's own commit call be moved anywhere relative
    # to it and still "pass" (a real regression: app_pc_release_slot + pc_bank_
    # restore_done moved ABOVE the ARM's commit still reads as after the UP path's
    # commit). Anchor the search at `m` (the PC->Bank arm's own memcpy) instead, so
    # `c` resolves to the arm's OWN commit call.
    c = first_match_line(lines, m if m is not None else start, end, COMMIT_RE)
    r = first_match_line(lines, start, end, PC_RELEASE_SLOT_RE)
    d = first_match_line(lines, start, end, RESTORE_DONE_RE)
    if u is None: return False, "drop_held: no `pc_bank_restore_up(` call in the PC->Bank arm"
    if m is None: return False, "drop_held: no PC->Bank ternary memcpy line found"
    if c is None: return False, "drop_held: no `ok = src->commit()` line in the PC->Bank arm"
    if r is None: return False, "drop_held: no `app_pc_release_slot(` call in the PC->Bank arm"
    if d is None: return False, "drop_held: no `pc_bank_restore_done(` call in the PC->Bank arm"
    if not u < m:
        return False, (f"drop_held: pc_bank_restore_up() (line {u+1}) does NOT come before "
                       f"the PC->Bank memcpy (line {m+1}) -- the rebuilt cell could not be "
                       f"ready in time to be written")
    if not (m < c and c < r and c < d and r < d):
        return False, (f"drop_held: pc_bank_restore_done() (line {d+1}) and "
                       f"app_pc_release_slot() (line {r+1}) must both come AFTER the PC->Bank "
                       f"arm's OWN src->commit() (line {c+1}, found after the arm's memcpy at "
                       f"line {m+1}) -- the entry would be marked (or the source released) "
                       f"before the Bank write it describes has actually landed")
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


# ---- (n) merged-tree review F4: bank_down_dispatch's three case->callee bindings.
# Shared by the real check (n) and its self-mutation demonstration (MUT O). ----------
DISPATCH_CASE_EXACT_RE    = re.compile(r"case\s+XG_DOWN_ARM_EXACT\s*:")
DISPATCH_CASE_GBBRIDGE_RE = re.compile(r"case\s+XG_DOWN_ARM_GB_BRIDGE\s*:")
DISPATCH_CASE_GEN3_RE     = re.compile(r"case\s+XG_DOWN_ARM_GEN3\s*:")
CALL_EXACT_RE    = re.compile(r"bank_down_exact\(")
CALL_GBBRIDGE_RE = re.compile(r"bank_down_convert_gb\(")
CALL_GEN3_RE     = re.compile(r"bank_down_convert_gen3\(")
DISPATCH_BINDINGS = (
    ("XG_DOWN_ARM_EXACT -> bank_down_exact(", DISPATCH_CASE_EXACT_RE, CALL_EXACT_RE),
    ("XG_DOWN_ARM_GB_BRIDGE -> bank_down_convert_gb(", DISPATCH_CASE_GBBRIDGE_RE, CALL_GBBRIDGE_RE),
    ("XG_DOWN_ARM_GEN3 -> bank_down_convert_gen3(", DISPATCH_CASE_GEN3_RE, CALL_GEN3_RE),
)


def dispatch_case_bindings(body: list[str]) -> list[tuple[bool, str]]:
    """bank_down_dispatch's own dense one-case-per-line switch style (source/pdna_box.c):
    each case's callee sits either on the case's own line or the next NON-BLANK line.
    Review F4: before this, only the switch's existence was pinned, never WHICH callee
    each case binds to -- a callee swap between two cases (MUT O) survived every other
    check in this file. Returns one (ok, msg) pair per binding, in DISPATCH_BINDINGS order."""
    results = []
    for label, case_re, call_re in DISPATCH_BINDINGS:
        case_i = first_match_line(body, 0, len(body), case_re)
        if case_i is None:
            results.append((False, f"bank_down_dispatch: case label not found for {label}"))
            continue
        ok = bool(call_re.search(body[case_i]))
        if not ok:
            j = case_i + 1
            while j < len(body) and body[j].strip() == "":
                j += 1
            ok = j < len(body) and bool(call_re.search(body[j]))
        results.append((ok, f"bank_down_dispatch: {label} not bound on its case line "
                             f"or the next non-blank line"))
    return results


# ---- (o) merged-tree review F4: drop_held's non-EXACT LANDED tail. Shared by the real
# check (o) and its self-mutation demonstrations (MUT P / MUT Q). --------------------
LANDED_IF_RE    = re.compile(r"if\s*\(\s*bd\s*==\s*BANK_DOWN_LANDED\s*\)\s*\{")
EXACT_ARM_IF_RE = re.compile(r"if\s*\(\s*arm\s*==\s*XG_DOWN_ARM_EXACT\s*\)\s*\{")
CLEAR_SLOTS_RE  = re.compile(r"app_bank_clear_slots\(")
DEFER_DELETE_RE = re.compile(r"app_bank_defer_delete\(")


def landed_tail_block(dh_body: list[str]) -> list[str]:
    """The non-EXACT LANDED tail inside drop_held: the block after
    `if (arm == XG_DOWN_ARM_EXACT) { ... return recs; }`, still inside
    `if (bd == BANK_DOWN_LANDED) { ... }` -- the GB_BRIDGE arm's own immediate Bank
    consume (59dd45c / merged-tree review F2)."""
    landed_i = first_match_line(dh_body, 0, len(dh_body), LANDED_IF_RE)
    if landed_i is None:
        raise AssertionError("drop_held: `if (bd == BANK_DOWN_LANDED) {` not found")
    depth = 0
    landed_end = landed_i
    for i in range(landed_i, len(dh_body)):
        depth += dh_body[i].count("{") - dh_body[i].count("}")
        if depth == 0 and i > landed_i:
            landed_end = i
            break
    landed_block = dh_body[landed_i:landed_end + 1]
    exact_i = first_match_line(landed_block, 0, len(landed_block), EXACT_ARM_IF_RE)
    if exact_i is None:
        raise AssertionError("drop_held: `if (arm == XG_DOWN_ARM_EXACT) {` not found "
                              "inside the LANDED block")
    depth = 0
    exact_end = exact_i
    for i in range(exact_i, len(landed_block)):
        depth += landed_block[i].count("{") - landed_block[i].count("}")
        if depth == 0 and i > exact_i:
            exact_end = i
            break
    return landed_block[exact_end + 1:]


# ---- (p) merged-tree review F4/F1: gb_bank_down_bridge's destination generation. ----
DST_GEN_LINE_RE = re.compile(r"uint8_t\s+dst_gen\s*=\s*g_ed->s\.gen\s*;")
INVERTED_GEN_RE = re.compile(r"\?\s*GB_GEN2\s*:\s*GB_GEN1")

# ---- (aa)/(ab)/(ac) BACKLOG #150 S150-10: gb_paste_hook's per-slot move rule (G-H8).
# Re-lettered from (u)/(v)/(w) at the merge with main/s150-12, which already owns
# those letters for its own three checks; s150-9 is taking (x)/(y)/(z).
# (aa) bad4 is computed (gb_clip_moves -> g3gb_moves_ok) BEFORE the first
# gen3_to_gb_fixed( call -- an uninitialised/stale bad4 handed to the converter would
# either wrongly refuse a legal record or (worse) wrongly empty one it never checked.
# (ab) the zero-move refusal (decision 8.7, the ONE exception to Guy's "never block")
# exists: all four slots bad AND the fill produced nothing must refuse before the
# modal, not silently hand a Struggle-forever mon to gb_paste_write.
# (ac) gb_paste_fill_moves( (the fill) is called BEFORE gb_paste_write( -- a bad slot
# that reached gen3_to_gb_fixed non-NULL must always be filled (or refused by (ab))
# before the record is ever written; a write reachable without the fill having run
# would leave a mid-list hole/an un-filled empty slot in the landed record. Shared by
# the real checks and their MUT X/MUT Y/MUT Z self-mutation demonstrations. ----------
CLIP_MOVES_RE      = re.compile(r"\bgb_clip_moves\(")
GEN3_TO_GB_FIXED_RE = re.compile(r"\bgen3_to_gb_fixed\(")
ZERO_MOVE_REFUSAL_RE = re.compile(r"nleft\s*==\s*0")
PASTE_FILL_MOVES_RE = re.compile(r"\bgb_paste_fill_moves\(")
PASTE_WRITE_CALL_RE = re.compile(r"\bgb_paste_write\(")

# ---- (ag) BACKLOG #210: s_busy_reading( must run BEFORE gb_paste_fill_moves( in
# gb_paste_hook -- the fill's own gb_create_locate_rom() call is a cold, uncached scan
# (decision 5's own choice), exactly like CREATE's, which CREATE masks with the same
# busy screen. Shared by the real check (ag) and its MUT AG self-mutation. (Re-lettered
# from (ad) at BACKLOG #212's re-verify R4 -- (ad) itself was reassigned to BACKLOG
# #168a review D2's own drop_held_up scan-latch check, main's check, NOT this one; see
# that check's own comment above, up_scan_latch_facts.) ------------------------------
BUSY_READING_RE = re.compile(r"\bs_busy_reading\(\)")

# ---- (aj)/(ah)/(ai)/(ak) BACKLOG #212: gb_bank_down_bridge's own per-slot move rule --
# the busy screen before the fill's cold ROM scan (aj), the fill before the modal (ah),
# the modal before the write (ai), and the same `nleft == 0` zero-move refusal (ab)
# gives gb_paste_hook (ak, BACKLOG #212 re-verify R3). Shared by the real checks and
# their MUT AJ/MUT AH/MUT AI self-mutation demonstrations (re-lettered from (ae)/(af)
# at R4; ak has no MUT of its own here -- it reuses ZERO_MOVE_REFUSAL_RE, whose own
# comment above documents its MUT Y demonstration for (ab)). -------------------------
LEGAL_SCREEN_EX_RE = re.compile(r"\bgb_paste_legal_screen_ex\(")
GBS_INSERT_CALL_RE = re.compile(r"\bgbs_insert\(")

# ---- (q)/(r) merged-tree reviewer: the non-EXACT LANDED tail must both repaint (q)
# and never re-admit a deferred delete even alongside a kept consume (r). Shared with
# MUT S / MUT T. -----------------------------------------------------------------
OAM_RELOAD_RE    = re.compile(r"s_oam_reload\s*=\s*true\s*;")
RECS_REASSIGN_RE = re.compile(r"recs\s*=\s*src->records\(box\)")


def landed_tail_repaints(tail: list[str]) -> tuple[bool, str]:
    """(q) merged-tree reviewer: the non-EXACT LANDED tail must repaint from the image
    after its Bank consume -- BOTH `s_oam_reload = true;` and a `recs =
    src->records(box)` reassignment. A revert that keeps the consume
    (app_bank_clear_slots() still present) but drops the repaint would leave the
    on-screen box stale after a GB_BRIDGE drop actually landed."""
    if not any(OAM_RELOAD_RE.search(ln) for ln in tail):
        return False, ("drop_held LANDED tail (non-EXACT): no `s_oam_reload = true;` "
                        "-- the on-screen box would go stale after a GB_BRIDGE drop")
    if not any(RECS_REASSIGN_RE.search(ln) for ln in tail):
        return False, ("drop_held LANDED tail (non-EXACT): no `recs = src->records(box)` "
                        "reassignment -- the caller keeps the pre-drop record image")
    return True, "ok"


def landed_tail_no_defer(tail: list[str]) -> tuple[bool, str]:
    """(r) merged-tree reviewer, negative form of review F2: the non-EXACT LANDED tail
    must contain NO app_bank_defer_delete( call, even if app_bank_clear_slots( is ALSO
    present -- a Game Boy session never runs the Gen-3 exit-save flush a deferred
    delete waits for, so re-ADDING a defer call alongside the immediate consume (not
    just replacing it) is just as wrong."""
    if any(DEFER_DELETE_RE.search(ln) for ln in tail):
        return False, ("drop_held LANDED tail (non-EXACT): contains "
                        "app_bank_defer_delete( -- a Game Boy session never runs the "
                        "Gen-3 exit-save flush a deferred delete waits for")
    return True, "ok"


# ---- (s)/(t) merged-tree reviewer: the LANDED tail's consume must come AFTER the
# dispatch call (s, MUT L's shape), and `arm` must be derived exactly ONCE, above that
# call (t). Both operate on drop_held's WHOLE body, not just the tail slice. Shared
# with MUT U / MUT V. ---------------------------------------------------------------
DISPATCH_CALL_RE = re.compile(r"\bbank_down_dispatch\(")
ARM_DERIVE_RE     = re.compile(r"\bxg_bank_down_arm\(")


def landed_consume_after_dispatch(dh_body: list[str]) -> tuple[bool, str]:
    """(s) merged-tree reviewer, MUT L's shape: drop_held's app_bank_clear_slots( call
    (the LANDED tail's Bank consume) must come AFTER the bank_down_dispatch( call in
    text order -- the same persist-before-consume ordering review F2 pinned for
    bank_down_exact, now pinned for drop_held's own caller-side consume too."""
    disp_i = first_match_line(dh_body, 0, len(dh_body), DISPATCH_CALL_RE)
    clear_i = first_match_line(dh_body, 0, len(dh_body), CLEAR_SLOTS_RE)
    if disp_i is None:
        return False, "drop_held: no bank_down_dispatch( call found"
    if clear_i is None:
        return False, "drop_held: no app_bank_clear_slots( call found"
    if not disp_i < clear_i:
        return False, (f"drop_held: app_bank_clear_slots( (line {clear_i + 1}) does not "
                        f"come AFTER bank_down_dispatch( (line {disp_i + 1}) -- the Bank "
                        f"slot would be consumed before the dispatch even runs")
    return True, "ok"


# BACKLOG #213 (al): every ledger-write function this lane could reach must call
# app_xv_cache_invalidate( somewhere in its own (comment-stripped) body -- the GB
# ORIGINAL row's negative cache promises "after ANY ledger write, the next lookup
# goes to the card", and this is the only site where THAT can be checked for the
# write functions that live in files with no host-compile path (pdna_gen12.c,
# pdna_box.c) or that are pdna_main.c statics with no runtime host test of their own.
INVALIDATE_RE = re.compile(r"\bapp_xv_cache_invalidate\(")


def invalidate_call_facts(lines: list[str], sig_re: str, fn_label: str) -> tuple[bool, str]:
    """(al): extract_function(lines, sig_re)'s body must contain INVALIDATE_RE at
    least once. MUT AL (self_test_mutation_detection) blanks the one real call line
    on a synthetic copy of app_xfer_promote's body and asserts this reports FAIL."""
    s, e = extract_function(lines, sig_re)
    body = lines[s:e]
    if not any(INVALIDATE_RE.search(ln) for ln in body):
        return False, (f"{fn_label}: no app_xv_cache_invalidate( call in its "
                        f"(comment-stripped) body -- a write here would leave the GB "
                        f"ORIGINAL row's cache stale after a real ledger change")
    return True, "ok"


def arm_derived_once_above_dispatch(dh_body: list[str]) -> tuple[bool, str]:
    """(t) merged-tree reviewer: drop_held derives `arm` via xg_bank_down_arm( exactly
    ONCE, above the bank_down_dispatch( call -- the union's original bug class was a
    second derivation inside the tail, which could disagree with the first if state
    changed in between."""
    arm_idx = [i for i, ln in enumerate(dh_body) if ARM_DERIVE_RE.search(ln)]
    disp_i = first_match_line(dh_body, 0, len(dh_body), DISPATCH_CALL_RE)
    if disp_i is None:
        return False, "drop_held: no bank_down_dispatch( call found"
    if len(arm_idx) != 1:
        return False, (f"drop_held: expected exactly 1 xg_bank_down_arm( call, "
                        f"found {len(arm_idx)}")
    if not arm_idx[0] < disp_i:
        return False, (f"drop_held: xg_bank_down_arm( (line {arm_idx[0] + 1}) does not "
                        f"come ABOVE bank_down_dispatch( (line {disp_i + 1})")
    return True, "ok"


def main() -> int:
    box_lines = strip_comments(BOX_C.read_text()).splitlines()
    box_text_stripped = "\n".join(box_lines)
    main_lines = strip_comments(MAIN_C.read_text()).splitlines()
    bank_lines = strip_comments(BANK_C.read_text()).splitlines()
    gen12_lines = strip_comments(GEN12_C.read_text()).splitlines()
    xferio_lines = strip_comments(XFER_IO_C.read_text()).splitlines()

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
    # calls release_up (the Game Boy delete) -- pins the order, not just presence.
    # BACKLOG #170: the UP branch is now its own noinline helper, drop_held_up --
    # re-anchored here in the same commit that moved it. ----
    s, e = extract_function(box_lines, r"^static uint8_t\* __attribute__\(\(noinline\)\) drop_held_up\(")
    ok, d = up_order_facts(box_lines, s, e)
    check(ok, d)

    # ---- (n1) BACKLOG #150 S150-12 decision 6: the release_up-optional wrap --
    # `if (s_xfer_peer->release_up)` after commit()/hand-empty, and app_pc_queue_note(
    # in its else, in that line order. BACKLOG #170: re-anchored to drop_held_up. ----
    s, e = extract_function(box_lines, r"^static uint8_t\* __attribute__\(\(noinline\)\) drop_held_up\(")
    ok, d = n1_order_facts(box_lines, s, e)
    check(ok, d)

    # ---- (l) BACKLOG #150 S150-8b: the RESTORE edge's ordering in drop_held's
    # PC->Bank arm -- pc_bank_restore_up( before the write, pc_bank_restore_done(
    # strictly after both the verified commit and the source release. ----
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    ok, d = restore_order_facts(box_lines, s, e)
    check(ok, d)

    # ---- (l') decision 13: app_can_edit( is the FIRST statement checked in both
    # pc_bank_restore_up() and pc_bank_restore_done() -- before any sf_write_verified(/
    # bc_pack( call, so a read-only cart refuses before touching the ledger at all. ----
    for name, sig, write_pat in [
        ("pc_bank_restore_up", r"^pc_bank_restore_up\(", XR_OPEN_RE),
        ("pc_bank_restore_done", r"^pc_bank_restore_done\(", SF_WRITE_VERIFIED_RE),
    ]:
        s, e = extract_function(box_lines, sig)
        ok, detail = gate_before_pattern(box_lines, s, e, APP_CAN_EDIT_RE, write_pat, name)
        check(ok, detail)

    # ---- (l'') review D8/F2: pc_bank_restore_done() must actually assign
    # XR_STATE_RESTORED -- pinned against the source directly, so a revert to the
    # gbsc_set_claimed() no-op (F2's original bug) is caught structurally, not just by
    # a host test that could itself regress unnoticed. ----
    s, e = extract_function(box_lines, r"^pc_bank_restore_done\(")
    pbrd_body = box_lines[s:e]
    check(any(XR_STATE_RESTORED_RE.search(ln) for ln in pbrd_body),
          "pc_bank_restore_done(): no `e.state = XR_STATE_RESTORED;` assignment found -- "
          "the RESTORED mark regressed to a no-op (review F2's original bug)")

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

    # ---- (n) merged-tree review F4: bank_down_dispatch's three case->callee bindings ----
    sdd, edd = extract_function(box_lines, r"^bank_down_dispatch\(")
    dispatch_body = box_lines[sdd:edd]
    for ok, msg in dispatch_case_bindings(dispatch_body):
        check(ok, msg)

    # ---- (o) merged-tree review F4: drop_held's non-EXACT LANDED tail must consume the
    # Bank slot IMMEDIATELY (app_bank_clear_slots(), 59dd45c), never defer it
    # (app_bank_defer_delete() -- a Game Boy session never runs the Gen-3 exit-save
    # flush a deferred delete waits for, merged-tree review F2). ----
    sh, eh = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    dh_body_full = box_lines[sh:eh]
    landed_tail = landed_tail_block(dh_body_full)
    check(any(CLEAR_SLOTS_RE.search(ln) for ln in landed_tail),
          "drop_held LANDED tail (non-EXACT): no app_bank_clear_slots( call -- the "
          "GB_BRIDGE arm's Bank slot would never be consumed")
    check(not any(DEFER_DELETE_RE.search(ln) for ln in landed_tail),
          "drop_held LANDED tail (non-EXACT): contains app_bank_defer_delete( -- a "
          "Game Boy session never runs the Gen-3 exit-save flush a deferred delete "
          "waits for (merged-tree review F2)")

    # ---- (q) merged-tree reviewer: the same tail must ALSO repaint (s_oam_reload +
    # recs reassignment) -- a revert that keeps the consume but drops the repaint. ----
    ok, msg = landed_tail_repaints(landed_tail)
    check(ok, msg)

    # ---- (r) merged-tree reviewer, negative form of F2: no app_bank_defer_delete( in
    # the tail, even alongside a kept app_bank_clear_slots(. ----
    ok, msg = landed_tail_no_defer(landed_tail)
    check(ok, msg)

    # ---- (s) merged-tree reviewer, MUT L's shape: the tail's consume comes AFTER the
    # bank_down_dispatch( call in text order. ----
    ok, msg = landed_consume_after_dispatch(dh_body_full)
    check(ok, msg)

    # ---- (t) merged-tree reviewer: `arm` is derived exactly ONCE, above the
    # bank_down_dispatch( call. ----
    ok, msg = arm_derived_once_above_dispatch(dh_body_full)
    check(ok, msg)

    # ---- (p) merged-tree review F4/F1: gb_bank_down_bridge's destination generation is
    # the MOUNTED session's own generation, never the inverted `? GB_GEN2 : GB_GEN1`
    # form review F1 removed. ----
    sb, eb = extract_function(gen12_lines, r"^BankDownResult gb_bank_down_bridge\(")
    bridge_body = gen12_lines[sb:eb]
    check(any(DST_GEN_LINE_RE.search(ln) for ln in bridge_body),
          "gb_bank_down_bridge: no `uint8_t dst_gen = g_ed->s.gen;` line found in its "
          "(comment-stripped) body")
    check(not any(INVERTED_GEN_RE.search(ln) for ln in bridge_body),
          "gb_bank_down_bridge: contains the inverted `? GB_GEN2 : GB_GEN1` form "
          "review F1 removed -- the bridge would land in the WRONG generation")
    check(sum(1 for ln in bridge_body if re.search(r"\bdst_gen\s*=", ln)) == 1,
          "gb_bank_down_bridge: dst_gen must be assigned exactly once (its declaration)")   # Fable review F1

    # ---- (x) BACKLOG #150 S150-9 decision 10: gb_lift_pack / gb_lift_restore's
    # ordering -- the cheap f_stat pre-check before the real restore open; inside
    # gb_lift_restore, the screen before the serial is spent, the serial before the
    # pack. (Re-lettered from the brief's drafting-time "(m)", then the lane's own
    # "(u)" -- both taken by lanes that landed on main first: s150-12's own (u)/(v)/
    # (w) below. Anchor updated post-merge: s150-12 folded gb_lift_up_hook's body
    # into the shared gb_lift_pack(rec80, out80, copy); the old
    # gb_lift_up_hook(...) signature no longer contains the gb_has_sidecar/
    # gb_lift_restore call pair at all.) ----
    sh, eh = extract_function(gen12_lines, r"^static bool gb_lift_pack\(const uint8_t\* rec80, uint8_t\* out80, bool copy\) \{")
    hook_body = gen12_lines[sh:eh]
    ok, d = lift_order_facts_hook(hook_body)
    check(ok, d)
    sr, er = extract_function(gen12_lines, r"^gb_lift_restore\(")
    restore_body = gen12_lines[sr:er]
    ok, d = lift_order_facts_restore(restore_body)
    check(ok, d)

    # ---- (y) BACKLOG #150 S150-9 decision 9: gb_release_up_hook's RESTORED
    # re-verify -- the ledger identity walk before the delete, the delete before
    # gb_persist(, the RESTORED mark strictly after gb_persist(. (Re-lettered from
    # the brief's drafting-time "(n)", then the lane's own "(v)" -- both taken.) ----
    s, e = extract_function(gen12_lines, r"^static bool gb_release_up_hook\(int box, int slot, const uint8_t cell80\[80\]\) \{")
    release_body = gen12_lines[s:e]
    ok, d = release_order_facts(release_body)
    check(ok, d)

    # ---- (z) BACKLOG #150 S150-9 decision 8: pc_bank_restore_up's state refusals
    # (RESTORED/PENDING) precede the merge screen, which precedes the serial spend.
    # (Re-lettered from the brief's drafting-time "(o)", then the lane's own "(w)" --
    # both taken.) ----
    s, e = extract_function(box_lines, r"^pc_bank_restore_up\(")
    restore_up_body = box_lines[s:e]
    ok, d = restore_up_state_before_screen_facts(restore_up_body)
    check(ok, d)

    # ---- (n3) BACKLOG #150 S150-12 decision 9: gb_bank_down_gen3/gb_bank_down_bridge
    # -- the `copy` derivation precedes the first ledger write, and every ledger call
    # in each body sits inside an `if (!copy` guard. ----
    for fn in ("gb_bank_down_gen3", "gb_bank_down_bridge"):
        ok, d = n3_facts(gen12_lines, fn)
        check(ok, d)

    # ---- (u)/(v)/(w) REVIEW FIX (LOW, BACKLOG #150 S150-12): three findings the
    # reviewer's own mutants M2/M7/M9 passed every check in this file today
    # (unpinned) -- see k_gb_xfer_ro_facts/have_xfer_facts/native_summary_mask_facts'
    # own docstrings above. ----
    ok, d = k_gb_xfer_ro_facts(gen12_lines)
    check(ok, d)
    ok, d = have_xfer_facts(box_lines)
    check(ok, d)
    ok, d = native_summary_mask_facts(gen12_lines)
    check(ok, d)

    # ---- (aa)/(ab)/(ac) BACKLOG #150 S150-10: gb_paste_hook's per-slot move rule --
    # RE-LETTERED from (u)/(v)/(w) (merge with main/s150-12, which already owns those
    # letters; s150-9 is taking (x)/(y)/(z), so (aa)/(ab)/(ac) are the next free
    # ones). ----
    sh, eh = extract_function(gen12_lines, r"^static bool gb_paste_hook\(")
    hook_body = gen12_lines[sh:eh]

    # (aa) bad4 is computed (gb_clip_moves) BEFORE the first gen3_to_gb_fixed( call.
    ok, msg = gate_before_pattern(hook_body, 0, len(hook_body), CLIP_MOVES_RE,
                                   GEN3_TO_GB_FIXED_RE, "gb_paste_hook")
    check(ok, msg)

    # (ab) the zero-move refusal (decision 8.7, review D1's `nleft == 0` predicate) exists.
    check(any(ZERO_MOVE_REFUSAL_RE.search(ln) for ln in hook_body),
          "gb_paste_hook: no `nleft == 0` zero-move refusal found in its (comment-stripped) "
          "body -- a mon that would be WRITTEN with no moves left at all (whether from all "
          "four slots bad, or 1-3 bad slots all out of range with the fill run dry) would "
          "land with no moves (Struggles forever, an illegal Game Boy record)")

    # (ac) the fill (gb_paste_fill_moves) is called BEFORE the write (gb_paste_write) --
    # a bad slot that reached gen3_to_gb_fixed non-NULL must always be filled (or the
    # whole paste refused by (ab)) before gb_paste_write ever runs.
    ok, msg = gate_before_pattern(hook_body, 0, len(hook_body), PASTE_FILL_MOVES_RE,
                                   PASTE_WRITE_CALL_RE, "gb_paste_hook")
    check(ok, msg)

    # ---- (ad) BACKLOG #168a review D2: drop_held_up's ident32-collision scan runs
    # once per session regardless of pdna_bank_serial_trusted() -- the `s_up_scan_done`
    # latch must be part of the same guard line, not a bare `!pdna_bank_serial_trusted()`
    # check. MUT AD below reverts it and must be caught. ----
    s, e = extract_function(box_lines, r"^static uint8_t\* __attribute__\(\(noinline\)\) drop_held_up\(")
    ok, d = up_scan_latch_facts(box_lines, s, e)
    check(ok, d)

    # (ag) BACKLOG #210: s_busy_reading( is called BEFORE gb_paste_fill_moves( -- the
    # fill's own gb_create_locate_rom() call is a cold, uncached, ~185,000-read scan
    # (same as CREATE's, which CREATE masks the same way).
    ok, msg = gate_before_pattern(hook_body, 0, len(hook_body), BUSY_READING_RE,
                                   PASTE_FILL_MOVES_RE, "gb_paste_hook")
    check(ok, msg)

    # ---- (aj)/(ah)/(ai) BACKLOG #212: gb_bank_down_bridge's own per-slot move rule --
    # the busy screen before the fill's cold ROM scan, the fill before the modal, the
    # modal before the write. ----
    sbr, ebr = extract_function(gen12_lines, r"^BankDownResult gb_bank_down_bridge\(")
    bridge_body = gen12_lines[sbr:ebr]

    # (aj) review D5: s_busy_reading( is called BEFORE gb_paste_fill_moves( in the
    # bridge too -- the bridge shares gb_paste_fill_moves' own cold, uncached,
    # ~185,000-read ROM scan (see (ag) above, gb_paste_hook's own copy of this check).
    ok, msg = gate_before_pattern(bridge_body, 0, len(bridge_body), BUSY_READING_RE,
                                   PASTE_FILL_MOVES_RE, "gb_bank_down_bridge")
    check(ok, msg)

    # (ah) gb_paste_fill_moves( is called BEFORE gb_paste_legal_screen_ex( (the modal).
    ok, msg = gate_before_pattern(bridge_body, 0, len(bridge_body), PASTE_FILL_MOVES_RE,
                                   LEGAL_SCREEN_EX_RE, "gb_bank_down_bridge")
    check(ok, msg)

    # (ai) gb_paste_legal_screen_ex( is called BEFORE gbs_insert( (the box write).
    ok, msg = gate_before_pattern(bridge_body, 0, len(bridge_body), LEGAL_SCREEN_EX_RE,
                                   GBS_INSERT_CALL_RE, "gb_bank_down_bridge")
    check(ok, msg)

    # (ak) BACKLOG #212 re-verify R3: the same `nleft == 0` zero-move refusal (ab)
    # requires of gb_paste_hook must exist in gb_bank_down_bridge too -- D9's own
    # zero-move guard (source/pdna_gen12.c ~:3852-3858), mirroring (ab) above.
    check(any(ZERO_MOVE_REFUSAL_RE.search(ln) for ln in bridge_body),
          "gb_bank_down_bridge: no `nleft == 0` zero-move refusal found in its "
          "(comment-stripped) body -- a bridge paste that would be WRITTEN with no "
          "moves left at all would land with no moves (Struggles forever, an "
          "illegal Game Boy record), the same D9 hole (ab) closes for gb_paste_hook")
    # ---- (al) BACKLOG #213: every ledger-write function this lane could reach calls
    # app_xv_cache_invalidate( -- the GB ORIGINAL row's negative cache promises "after
    # ANY ledger write the next lookup goes to the card"; this is the only place that
    # can prove it for the write sites living in files with no host-compile path
    # (pdna_gen12.c/pdna_box.c) or that are pdna_main.c statics with no runtime test of
    # their own. MUT AL below (self_test_mutation_detection) blanks the one real call
    # line in a synthetic copy of app_xfer_promote's body and must be caught. ----
    for lines, sig_re, label in (
        (main_lines, r"^bool __attribute__\(\(noinline\)\) app_xfer_promote\(void\) \{", "app_xfer_promote"),
        (main_lines, r"^static void __attribute__\(\(noinline\)\) app_xfer_pid_rekey\(",
         "app_xfer_pid_rekey"),
        (main_lines, r"^static void __attribute__\(\(noinline\)\) xfer_reconcile_apply\(",
         "xfer_reconcile_apply"),
        (xferio_lines, r"^static bool write_marker\(uint32_t copied\) \{", "xfer_io.c write_marker"),
        (gen12_lines, r"^xfer_down_write\(uint64_t key", "xfer_down_write"),
        (gen12_lines, r"^static void xfer_down_claim_now\(", "xfer_down_claim_now"),
        (gen12_lines, r"^static void xfer_down_undo\(", "xfer_down_undo"),
        (gen12_lines, r"^static bool gb_release_up_hook\(int box, int slot, const uint8_t cell80\[80\]\) \{",
         "gb_release_up_hook"),
        (box_lines, r"^pc_bank_restore_done\(const uint8_t g3_rec80\[80\]\) \{", "pc_bank_restore_done"),
        (gen12_lines, r"^static bool __attribute__\(\(noinline\)\) gb_paste_write\(", "gb_paste_write"),
    ):
        ok, d = invalidate_call_facts(lines, sig_re, label)
        check(ok, d)

    # ---- (f) review F3: the self-mutation harness, every run ----
    self_test_mutation_detection(box_lines, gen12_lines)
    self_test_al_mutation(main_lines)

    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


def self_test_mutation_detection(box_lines: list[str], gen12_lines: list[str]) -> None:
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
    # line in a copy of drop_held_up's body (BACKLOG #170: re-anchored -- the UP
    # branch moved into its own noinline helper) -- the exact defect the reviewer
    # demonstrated (the Game Boy save would lose the mon before the Bank has
    # committed it) -- and assert up_order_facts() reports failure.
    s, e = extract_function(box_lines, r"^static uint8_t\* __attribute__\(\(noinline\)\) drop_held_up\(")
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

    # MUT N1 (BACKLOG #150 S150-12 decision 6): move the `app_pc_queue_note(` line to
    # ABOVE `ok = src->commit()` in a copy of drop_held_up's body (BACKLOG #170:
    # re-anchored, same reason as MUT H above) -- the copy would be queued for the PC
    # offer before the Bank write that supposedly landed it has even been attempted --
    # and assert n1_order_facts() reports failure.
    s, e = extract_function(box_lines, r"^static uint8_t\* __attribute__\(\(noinline\)\) drop_held_up\(")
    body = box_lines[s:e]
    commit_i3 = first_match_line(body, 0, len(body), COMMIT_RE)
    queue_i = first_match_line(body, 0, len(body), PC_QUEUE_NOTE_RE)
    check(commit_i3 is not None and queue_i is not None,
          "MUT N1: could not locate both the commit() and app_pc_queue_note() lines in the real source -- fix this test")
    if commit_i3 is not None and queue_i is not None and commit_i3 < queue_i:
        mut_n1 = list(body)
        queue_line = mut_n1.pop(queue_i)
        mut_n1.insert(commit_i3, queue_line)   # app_pc_queue_note's line now sits BEFORE commit()
        ok, detail = n1_order_facts(mut_n1, 0, len(mut_n1))
        check(not ok, f"MUT N1 (app_pc_queue_note moved above commit()) should have been caught but was not: {detail}")
        print(f"  MUT N1 demonstration -- app_pc_queue_note() line moved above src->commit(): {detail}")

    # MUT K (BACKLOG #150 S150-8b, step 5's own demonstration): move
    # pc_bank_restore_done( ABOVE `ok = src->commit()` in a copy of drop_held's body --
    # the entry would be marked as restored before the Bank write it describes has
    # actually landed -- and assert restore_order_facts() reports failure.
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    body = box_lines[s:e]
    commit_i2 = first_match_line(body, 0, len(body), COMMIT_RE)
    done_i = first_match_line(body, 0, len(body), RESTORE_DONE_RE)
    check(commit_i2 is not None and done_i is not None,
          "MUT K: could not locate both the commit() and pc_bank_restore_done() lines in the real source -- fix this test")
    if commit_i2 is not None and done_i is not None and commit_i2 < done_i:
        mut_k = list(body)
        done_line = mut_k.pop(done_i)
        mut_k.insert(commit_i2, done_line)   # pc_bank_restore_done's line now sits BEFORE commit()
        ok, detail = restore_order_facts(mut_k, 0, len(mut_k))
        check(not ok, f"MUT K (pc_bank_restore_done swapped before commit()) should have been caught but was not: {detail}")
        print(f"  MUT K demonstration -- pc_bank_restore_done() line swapped above src->commit(): {detail}")

    # MUT N (review D4): move BOTH app_pc_release_slot( AND pc_bank_restore_done(
    # above the PC->Bank ARM's own `ok = src->commit()` (found via RESTORE_MEMCPY_RE,
    # the same anchor the fixed restore_order_facts() uses) -- the exact shape the
    # PRE-D4 checker (anchored on the UP path's FIRST commit() match, much earlier in
    # the function) could not catch, because both moved lines would still sit textually
    # AFTER that earlier, unrelated commit() line and read as "ok".
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    body = box_lines[s:e]
    m_i = first_match_line(body, 0, len(body), RESTORE_MEMCPY_RE)
    arm_commit_i = first_match_line(body, m_i if m_i is not None else 0, len(body), COMMIT_RE)
    release_i2 = first_match_line(body, 0, len(body), PC_RELEASE_SLOT_RE)
    done_i2 = first_match_line(body, 0, len(body), RESTORE_DONE_RE)
    check(m_i is not None and arm_commit_i is not None and release_i2 is not None and done_i2 is not None,
          "MUT N: could not locate the memcpy/arm-commit/release/done lines in the real source -- fix this test")
    if (m_i is not None and arm_commit_i is not None and release_i2 is not None and done_i2 is not None
            and arm_commit_i < release_i2 and arm_commit_i < done_i2):
        mut_n = list(body)
        # pop the LATER index first so the earlier index stays valid.
        first_pop, second_pop = sorted([release_i2, done_i2], reverse=True)
        line_a = mut_n.pop(first_pop)
        line_b = mut_n.pop(second_pop)
        mut_n.insert(arm_commit_i, line_a)
        mut_n.insert(arm_commit_i, line_b)
        ok, detail = restore_order_facts(mut_n, 0, len(mut_n))
        check(not ok, f"MUT N (both app_pc_release_slot and pc_bank_restore_done swapped "
                       f"above the ARM's own commit()) should have been caught but was not: {detail}")
        print(f"  MUT N demonstration -- app_pc_release_slot() AND pc_bank_restore_done() "
              f"both swapped above the PC->Bank arm's own src->commit(): {detail}")

    # MUT O (review D8/F2): revert pc_bank_restore_done()'s `e.state = XR_STATE_RESTORED;`
    # line to a bare `gbsc_set_claimed(buf, len, best, true)` call (F2's original,
    # byte-for-byte no-op bug -- xfer_down_write already sets claimed=1 at birth) and
    # assert the (l'') check catches it.
    s, e = extract_function(box_lines, r"^pc_bank_restore_done\(")
    pbrd_body = list(box_lines[s:e])
    mut_o = [ln for ln in pbrd_body if not XR_STATE_RESTORED_RE.search(ln)]
    found_state_line = len(mut_o) != len(pbrd_body)
    check(found_state_line, "MUT O: could not find `e.state = XR_STATE_RESTORED;` in the real source -- fix this test")
    if found_state_line:
        ok = any(XR_STATE_RESTORED_RE.search(ln) for ln in mut_o)
        check(not ok, "MUT O (XR_STATE_RESTORED assignment removed) should have been caught but was not")
        print(f"  MUT O demonstration -- `e.state = XR_STATE_RESTORED;` removed from "
              f"pc_bank_restore_done(): correctly caught (no such assignment found)")

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

    # MUT S (BACKLOG #142, lane b142): revert the entry-time `st == 1 && !s_holding`
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
    check(reverted_n, "MUT S: could not find the real fixed arrival site to revert -- "
                       "fix this test")
    if reverted_n:
        ok, sites = arrival_never_saves_check(mut_n)
        check(not ok, f"MUT S (arrival site reverted to `is_bank ? 2 : 1`) should have "
                       f"been caught but was not: {sites}")
        print(f"  MUT S demonstration -- arrival site reverted to the pre-fix "
              f"`is_bank ? 2 : 1` ternary: correctly caught ({sites})")

    # MUT O (merged-tree review F4): swap bank_down_dispatch's GB_BRIDGE and GEN3
    # callees on a copy -- the switch still compiles and still has a case for each arm,
    # but a GB_BRIDGE drop would land inside bank_down_convert_gen3() and vice versa.
    s, e = extract_function(box_lines, r"^bank_down_dispatch\(")
    body = box_lines[s:e]
    mut_o = list(body)
    gb_i = first_match_line(mut_o, 0, len(mut_o), DISPATCH_CASE_GBBRIDGE_RE)
    g3_i = first_match_line(mut_o, 0, len(mut_o), DISPATCH_CASE_GEN3_RE)
    check(gb_i is not None and g3_i is not None,
          "MUT O: could not locate both the GB_BRIDGE and GEN3 case lines to swap -- fix this test")
    if gb_i is not None and g3_i is not None:
        placeholder = "\x00MUT_O_SWAP\x00"
        mut_o[gb_i] = CALL_GBBRIDGE_RE.sub(placeholder, mut_o[gb_i])
        mut_o[gb_i] = CALL_GEN3_RE.sub("bank_down_convert_gb(", mut_o[gb_i])
        mut_o[gb_i] = mut_o[gb_i].replace(placeholder, "bank_down_convert_gen3(")
        mut_o[g3_i] = CALL_GEN3_RE.sub(placeholder, mut_o[g3_i])
        mut_o[g3_i] = CALL_GBBRIDGE_RE.sub("bank_down_convert_gen3(", mut_o[g3_i])
        mut_o[g3_i] = mut_o[g3_i].replace(placeholder, "bank_down_convert_gb(")
        results = dispatch_case_bindings(mut_o)
        any_fail = any(not ok for ok, _ in results)
        check(any_fail, "MUT O (GB_BRIDGE/GEN3 callees swapped) should have been caught but was not")
        print(f"  MUT O demonstration -- bank_down_dispatch's GB_BRIDGE/GEN3 callees swapped: "
              f"{[msg for ok, msg in results if not ok]}")

    # MUT P (merged-tree review F4): delete drop_held's non-EXACT LANDED-tail
    # app_bank_clear_slots( call on a copy -- the GB_BRIDGE arm's Bank slot would never
    # be consumed, a permanent duplicate.
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    dh_body = box_lines[s:e]
    tail = landed_tail_block(dh_body)
    check(any(CLEAR_SLOTS_RE.search(ln) for ln in tail),
          "MUT P: the real drop_held LANDED tail has no app_bank_clear_slots( call to "
          "delete -- fix this test")
    mut_p = [ln for ln in tail if not CLEAR_SLOTS_RE.search(ln)]
    ok = any(CLEAR_SLOTS_RE.search(ln) for ln in mut_p)
    check(not ok, "MUT P (app_bank_clear_slots( deleted from the LANDED tail) should "
                   "have been caught but was not")
    print("  MUT P demonstration -- app_bank_clear_slots( deleted from drop_held's "
          "non-EXACT LANDED tail: correctly caught")

    # MUT Q (merged-tree review F4): replace that same call with
    # app_bank_defer_delete( on a copy -- the GB_BRIDGE arm would wait for a Gen-3
    # exit-save flush a Game Boy session never runs (merged-tree review F2).
    mut_q = [CLEAR_SLOTS_RE.sub("app_bank_defer_delete(", ln) for ln in tail]
    still_has_clear = any(CLEAR_SLOTS_RE.search(ln) for ln in mut_q)
    now_has_defer = any(DEFER_DELETE_RE.search(ln) for ln in mut_q)
    check(not still_has_clear and now_has_defer,
          "MUT Q: substitution did not produce the expected app_bank_defer_delete( "
          "shape -- fix this test")
    check(now_has_defer, "MUT Q (app_bank_clear_slots( replaced with "
                          "app_bank_defer_delete() should have been caught but was not")
    print("  MUT Q demonstration -- app_bank_clear_slots( replaced with "
          "app_bank_defer_delete( in drop_held's non-EXACT LANDED tail: correctly caught")

    # MUT R (merged-tree review F1): restore gb_bank_down_bridge's pre-fix inverted
    # `? GB_GEN2 : GB_GEN1` destination-generation line on a copy -- review F1 found
    # this bridges into the WRONG generation.
    s, e = extract_function(gen12_lines, r"^BankDownResult gb_bank_down_bridge\(")
    bridge_body = gen12_lines[s:e]
    dst_i = first_match_line(bridge_body, 0, len(bridge_body), DST_GEN_LINE_RE)
    check(dst_i is not None,
          "MUT R: could not locate the real `uint8_t dst_gen = g_ed->s.gen;` line -- fix this test")
    if dst_i is not None:
        mut_r = list(bridge_body)
        mut_r[dst_i] = "  uint8_t dst_gen = (g_m->kind == GB12_SAVE_RBY) ? GB_GEN2 : GB_GEN1;"
        ok = not any(INVERTED_GEN_RE.search(ln) for ln in mut_r)
        check(not ok, "MUT R (inverted `? GB_GEN2 : GB_GEN1` restored) should have "
                       "been caught but was not")
        print("  MUT R demonstration -- gb_bank_down_bridge's dst_gen line reverted to "
              "the inverted `? GB_GEN2 : GB_GEN1` form: correctly caught")

    # MUT N3 (BACKLOG #150 S150-12 decision 9): delete the `if (!copy)` guard around
    # gb_bank_down_gen3's Gen-3 ledger-write arm on a copy of gb_bank_down_gen3's
    # body -- the exact regression the brief's STOP-LICENCE item 6 names (a copy cell
    # reaching xfer_down_write on any arm) -- and assert n3_facts() reports failure.
    s, e = extract_function(gen12_lines, r"^BankDownResult gb_bank_down_gen3\(")
    gen3_body = gen12_lines[s:e]
    # the guard that actually ENCLOSES xfer_down_write( -- not the earlier, unrelated
    # `if (!copy && app_xfer_pending())` SAVE-FIRST pre-flight, which also matches
    # COPY_GUARD_RE but guards a REFUSAL, not the ledger write. Search backward from
    # the write call for the nearest block-opening `if (!copy) {`.
    write_i = first_match_line(gen3_body, 0, len(gen3_body), XFER_DOWN_WRITE_RE)
    check(write_i is not None, "MUT N3: could not locate xfer_down_write( in gb_bank_down_gen3's real source -- fix this test")
    guard_i = None
    if write_i is not None:
        for j in range(write_i, -1, -1):
            if COPY_GUARD_RE.search(gen3_body[j]) and "{" in gen3_body[j]:
                guard_i = j
                break
    check(guard_i is not None,
          "MUT N3: could not locate the `if (!copy) {` guard enclosing xfer_down_write( in gb_bank_down_gen3's real source -- fix this test")
    if guard_i is not None and "{" in gen3_body[guard_i]:
        # delete just the guard LINE itself (leaving its `{`-opened block's own lines
        # and its matching `}` in place) -- an unbalanced brace is fine for this
        # heuristic checker (it only re-scans the same 4 call patterns against a
        # depth walk that starts from 0 regardless), and matches the shape of every
        # other single-line-deletion MUT in this file.
        mut_n3 = gen3_body[:guard_i] + gen3_body[guard_i + 1:]
        ok, detail = n3_facts_over_body(mut_n3, "gb_bank_down_gen3")
        check(not ok, f"MUT N3 (the `if (!copy) {{` guard deleted around the Gen-3 ledger "
                       f"write) should have been caught but was not: {detail}")
        print(f"  MUT N3 demonstration -- `if (!copy) {{` guard deleted from "
              f"gb_bank_down_gen3's ledger-write arm: {detail}")

    # MUT W (merged-tree reviewer): drop the non-EXACT LANDED tail's repaint lines
    # (s_oam_reload = true; and the recs = src->records(box) reassignment) on a copy,
    # keeping the Bank consume -- a revert that keeps the consume but drops the
    # repaint would leave the on-screen box stale after a GB_BRIDGE drop.
    s, e = extract_function(box_lines, r"^static uint8_t\* drop_held\(")
    dh_body = box_lines[s:e]
    tail = landed_tail_block(dh_body)
    ok, _ = landed_tail_repaints(tail)
    check(ok, "MUT W: the real drop_held LANDED tail does not currently repaint -- fix this test")
    mut_s = [ln for ln in tail if not OAM_RELOAD_RE.search(ln) and not RECS_REASSIGN_RE.search(ln)]
    ok2, detail = landed_tail_repaints(mut_s)
    check(not ok2, f"MUT W (repaint lines dropped from the LANDED tail) should have "
                    f"been caught but was not: {detail}")
    print(f"  MUT W demonstration -- s_oam_reload/recs reassignment dropped from "
          f"drop_held's non-EXACT LANDED tail: {detail}")

    # MUT T (merged-tree reviewer): re-ADD app_bank_defer_delete( into the tail on a
    # copy WITHOUT removing app_bank_clear_slots( -- the regression is re-adding the
    # defer, not just replacing the immediate consume (that shape is MUT Q).
    mut_t = list(tail) + ["        app_bank_defer_delete(s_orig_box, s_orig_slot);"]
    ok3, detail = landed_tail_no_defer(mut_t)
    check(not ok3, f"MUT T (app_bank_defer_delete( re-added alongside a kept "
                    f"app_bank_clear_slots() should have been caught but was not: {detail}")
    print(f"  MUT T demonstration -- app_bank_defer_delete( re-added alongside a kept "
          f"app_bank_clear_slots( in the LANDED tail: {detail}")

    # MUT U (merged-tree reviewer, MUT L's shape): move drop_held's
    # app_bank_clear_slots( call line to BEFORE its bank_down_dispatch( call on a
    # copy -- the Bank slot would be consumed before the dispatch that decides whether
    # the drop even lands.
    disp_i = first_match_line(dh_body, 0, len(dh_body), DISPATCH_CALL_RE)
    clear_i = first_match_line(dh_body, 0, len(dh_body), CLEAR_SLOTS_RE)
    check(disp_i is not None and clear_i is not None and disp_i < clear_i,
          "MUT U: could not locate bank_down_dispatch( before app_bank_clear_slots( "
          "in the real source -- fix this test")
    if disp_i is not None and clear_i is not None and disp_i < clear_i:
        mut_u = list(dh_body)
        clear_line = mut_u.pop(clear_i)
        mut_u.insert(disp_i, clear_line)   # the Bank consume's line now sits BEFORE the dispatch call
        ok4, detail = landed_consume_after_dispatch(mut_u)
        check(not ok4, f"MUT U (app_bank_clear_slots( moved before bank_down_dispatch() "
                        f"should have been caught but was not: {detail}")
        print(f"  MUT U demonstration -- app_bank_clear_slots( line moved above "
              f"bank_down_dispatch(: {detail}")

    # MUT V (merged-tree reviewer): duplicate drop_held's `arm = xg_bank_down_arm(...)`
    # derivation into a second site just after the bank_down_dispatch( call, on a
    # copy -- the union's original bug class was a second derivation in the tail,
    # which could disagree with the first if state changed in between.
    arm_idx = [i for i, ln in enumerate(dh_body) if ARM_DERIVE_RE.search(ln)]
    check(len(arm_idx) == 1, f"MUT V: expected exactly 1 real xg_bank_down_arm( call to "
                              f"duplicate, found {len(arm_idx)} -- fix this test")
    if len(arm_idx) == 1 and disp_i is not None:
        mut_v = list(dh_body)
        mut_v.insert(disp_i + 1, mut_v[arm_idx[0]])   # a second derivation, now AFTER dispatch too
        ok5, detail = arm_derived_once_above_dispatch(mut_v)
        check(not ok5, f"MUT V (a second xg_bank_down_arm( derivation added after "
                        f"dispatch) should have been caught but was not: {detail}")
        print(f"  MUT V demonstration -- a second xg_bank_down_arm( derivation added "
              f"after bank_down_dispatch(: {detail}")

    # MUT X (BACKLOG #150 S150-9 decision 10, check (u)'s own demonstration): move
    # gb_lift_restore's app_xfer_merge_screen( call to AFTER pdna_bank_next_serial( --
    # a refused screen (B) would then have already spent a serial for nothing.
    sr, er = extract_function(gen12_lines, r"^gb_lift_restore\(")
    restore_body_real = gen12_lines[sr:er]
    scr_i = first_match_line(restore_body_real, 0, len(restore_body_real), APP_XFER_MERGE_SCREEN_RE)
    ser_i = first_match_line(restore_body_real, 0, len(restore_body_real), BANK_NEXT_SERIAL_RE)
    check(scr_i is not None and ser_i is not None and scr_i < ser_i,
          "MUT X: could not locate app_xfer_merge_screen(/pdna_bank_next_serial( in the "
          "real gb_lift_restore source in the expected order -- fix this test")
    if scr_i is not None and ser_i is not None and scr_i < ser_i:
        mut_x = list(restore_body_real)
        scr_line = mut_x.pop(scr_i)
        mut_x.insert(ser_i, scr_line)   # the screen call now sits AFTER the serial spend
        ok, detail = lift_order_facts_restore(mut_x)
        check(not ok, f"MUT X (app_xfer_merge_screen( moved after pdna_bank_next_serial() "
                       f"should have been caught but was not: {detail}")
        print(f"  MUT X demonstration -- gb_lift_restore's screen call moved after the "
              f"serial spend: {detail}")

    # MUT Y (BACKLOG #150 S150-9 decision 9, check (v)'s own demonstration): move the
    # `e.state = XR_STATE_RESTORED;` mark line to BEFORE gb_persist( in a copy of
    # gb_release_up_hook's body -- the entry would be marked restored before the
    # Game Boy save that makes it true has actually landed.
    s, e = extract_function(gen12_lines, r"^static bool gb_release_up_hook\(int box, int slot, const uint8_t cell80\[80\]\) \{")
    release_body_real = gen12_lines[s:e]
    p_i = first_match_line(release_body_real, 0, len(release_body_real), GB_PERSIST_RE)
    m_i = first_match_line(release_body_real, 0, len(release_body_real), XR_STATE_RESTORED_RE)
    check(p_i is not None and m_i is not None and p_i < m_i,
          "MUT Y: could not locate gb_persist(/the RESTORED mark in the real "
          "gb_release_up_hook source in the expected order -- fix this test")
    if p_i is not None and m_i is not None and p_i < m_i:
        mut_y = list(release_body_real)
        mark_line = mut_y.pop(m_i)
        mut_y.insert(p_i, mark_line)   # the mark now sits BEFORE gb_persist(
        ok, detail = release_order_facts(mut_y)
        check(not ok, f"MUT Y (the RESTORED mark moved before gb_persist() should have "
                       f"been caught but was not: {detail}")
        print(f"  MUT Y demonstration -- gb_release_up_hook's RESTORED mark line swapped "
              f"above gb_persist(: {detail}")

    # MUT Z (BACKLOG #150 S150-9 decision 8, check (w)'s own demonstration): move the
    # `e.state == XR_STATE_RESTORED` refusal check to AFTER app_xfer_merge_screen( in
    # a copy of pc_bank_restore_up's body -- the screen (and a serial spend) would run
    # before the state refusal ever gets a chance to stop it.
    s, e = extract_function(box_lines, r"^pc_bank_restore_up\(")
    restore_up_body_real = box_lines[s:e]
    rst_i = first_match_line(restore_up_body_real, 0, len(restore_up_body_real), RESTORE_UP_RESTORED_CHECK_RE)
    scr2_i = first_match_line(restore_up_body_real, 0, len(restore_up_body_real), APP_XFER_MERGE_SCREEN_RE)
    check(rst_i is not None and scr2_i is not None and rst_i < scr2_i,
          "MUT Z: could not locate the RESTORED state check/app_xfer_merge_screen( in "
          "the real pc_bank_restore_up source in the expected order -- fix this test")
    if rst_i is not None and scr2_i is not None and rst_i < scr2_i:
        mut_z = list(restore_up_body_real)
        # Find the whole `if (e.state == XR_STATE_RESTORED) { ... }` block's start line
        # and move JUST that condition line (the check itself) below the screen call --
        # enough to trip the ordering fact even though the rest of the block trails it.
        cond_line = mut_z.pop(rst_i)
        mut_z.insert(scr2_i, cond_line)
        ok, detail = restore_up_state_before_screen_facts(mut_z)
        check(not ok, f"MUT Z (the RESTORED state check moved after "
                       f"app_xfer_merge_screen() should have been caught but was not: {detail}")
        print(f"  MUT Z demonstration -- pc_bank_restore_up's RESTORED state check line "
              f"swapped below app_xfer_merge_screen(: {detail}")
    # MUT M2 (REVIEW FIX, LOW, BACKLOG #150 S150-12): give k_gb_xfer_ro's literal a
    # non-NULL release_up in a copy of gen12_lines -- must go red.
    idx = first_match_line(gen12_lines, 0, len(gen12_lines), K_GB_XFER_RO_RE)
    check(idx is not None, "MUT M2: could not locate k_gb_xfer_ro's literal in the real source -- fix this test")
    if idx is not None:
        target_m2 = "  .release_up = 0, .move_within = 0,"
        check(gen12_lines[idx + 2] == target_m2,
              f"MUT M2: k_gb_xfer_ro's third line does not match the expected "
              f"{target_m2!r} (found {gen12_lines[idx + 2]!r}) -- fix this test")
        if gen12_lines[idx + 2] == target_m2:
            mut_m2 = list(gen12_lines)
            mut_m2[idx + 2] = "  .release_up = gb_release_up_hook, .move_within = 0,"
            ok_m2, detail = k_gb_xfer_ro_facts(mut_m2)
            check(not ok_m2, f"MUT M2 (k_gb_xfer_ro given a non-NULL release_up) should have "
                              f"been caught but was not: {detail}")
            print(f"  MUT M2 demonstration -- k_gb_xfer_ro's .release_up set to "
                  f"gb_release_up_hook: {detail}")

    # MUT M7 (REVIEW FIX, LOW, BACKLOG #150 S150-12): add `&& s_xfer_peer->release_up`
    # back onto drop_held's `have_xfer` line in a copy of box_lines -- must go red.
    idx7 = first_match_line(box_lines, 0, len(box_lines), HAVE_XFER_RE)
    check(idx7 is not None, "MUT M7: could not locate the `have_xfer` line in the real source -- fix this test")
    if idx7 is not None:
        mut_m7 = list(box_lines)
        mut_m7[idx7] = mut_m7[idx7].rstrip().rstrip(";") + " && s_xfer_peer->release_up;"
        ok_m7, detail = have_xfer_facts(mut_m7)
        check(not ok_m7, f"MUT M7 (`&& s_xfer_peer->release_up` re-added to have_xfer) should "
                          f"have been caught but was not: {detail}")
        print(f"  MUT M7 demonstration -- `&& s_xfer_peer->release_up` re-added to "
              f"drop_held's have_xfer line: {detail}")

    # MUT M9 (REVIEW FIX, LOW, BACKLOG #150 S150-12): drop `| BC_FLAG_COPY` from
    # gb_native_summary_open's `nf` re-pack mask in a copy of gen12_lines -- must go red.
    idx9 = first_match_line(gen12_lines, 0, len(gen12_lines), NF_MASK_RE)
    check(idx9 is not None, "MUT M9: could not locate the `nf` re-pack mask line in the real source -- fix this test")
    if idx9 is not None:
        window9 = gen12_lines[idx9:idx9 + 3]
        found9 = [i for i, ln in enumerate(window9) if "BC_FLAG_COPY" in ln]
        check(len(found9) == 1, f"MUT M9: expected exactly 1 line naming BC_FLAG_COPY in the "
                                 f"3-line mask window, found {len(found9)} -- fix this test")
        if len(found9) == 1:
            mut_m9 = list(gen12_lines)
            j = idx9 + found9[0]
            mut_m9[j] = mut_m9[j].replace("BC_FLAG_COPY", "").replace(" |  |", " |").replace("| )", ")")
            ok_m9, detail = native_summary_mask_facts(mut_m9)
            check(not ok_m9, f"MUT M9 (BC_FLAG_COPY dropped from the nf re-pack mask) should "
                              f"have been caught but was not: {detail}")
            print(f"  MUT M9 demonstration -- BC_FLAG_COPY dropped from "
                  f"gb_native_summary_open's nf re-pack mask: {detail}")

    # BACKLOG #150 S150-10: gb_paste_hook's per-slot move rule -- MUT X/Y/Z. Checks
    # RE-LETTERED to (aa)/(ab)/(ac) above (merge with main/s150-12); the MUT letters
    # themselves are untouched (X/Y/Z were already free, no collision).
    sh, eh = extract_function(gen12_lines, r"^static bool gb_paste_hook\(")
    hook_body = gen12_lines[sh:eh]

    # MUT X: delete the `gb_clip_moves(` call line on a copy -- (aa) must fail to find
    # bad4 computed at all, not silently accept an uninitialised bad4.
    clip_i = first_match_line(hook_body, 0, len(hook_body), CLIP_MOVES_RE)
    check(clip_i is not None, "MUT X: could not locate the real gb_clip_moves( call in "
                               "gb_paste_hook -- fix this test")
    if clip_i is not None:
        mut_x = [ln for i, ln in enumerate(hook_body) if i != clip_i]
        ok6, detail = gate_before_pattern(mut_x, 0, len(mut_x), CLIP_MOVES_RE,
                                           GEN3_TO_GB_FIXED_RE, "gb_paste_hook (MUT X)")
        check(not ok6, f"MUT X (gb_clip_moves( deleted) should have been caught but was "
                        f"not: {detail}")
        print(f"  MUT X demonstration -- gb_clip_moves( call deleted from gb_paste_hook: {detail}")

    # MUT Y: replace the zero-move refusal's condition with a weaker one (decision
    # 8.7's ONE exception to "never block" silently disappears) on a copy -- (ab) must
    # fail to find it.
    zero_i = first_match_line(hook_body, 0, len(hook_body), ZERO_MOVE_REFUSAL_RE)
    check(zero_i is not None, "MUT Y: could not locate the real `nleft == 0` refusal "
                               "in gb_paste_hook -- fix this test")
    if zero_i is not None:
        mut_y = list(hook_body)
        mut_y[zero_i] = ZERO_MOVE_REFUSAL_RE.sub("false", mut_y[zero_i])
        ok7 = any(ZERO_MOVE_REFUSAL_RE.search(ln) for ln in mut_y)
        check(not ok7, "MUT Y (zero-move refusal condition weakened to `false`) should "
                        "have been caught but was not")
        print("  MUT Y demonstration -- `nleft == 0` replaced with `false` "
              "in gb_paste_hook: correctly caught")

    # MUT Z: move the `gb_paste_fill_moves(` call line to AFTER `gb_paste_write(` on a
    # copy -- (ac) must fail: a bad slot could reach the write un-filled.
    fill_i = first_match_line(hook_body, 0, len(hook_body), PASTE_FILL_MOVES_RE)
    write_i = first_match_line(hook_body, 0, len(hook_body), PASTE_WRITE_CALL_RE)
    check(fill_i is not None and write_i is not None and fill_i < write_i,
          "MUT Z: could not locate gb_paste_fill_moves( before gb_paste_write( in the "
          "real source -- fix this test")
    if fill_i is not None and write_i is not None and fill_i < write_i:
        mut_z = list(hook_body)
        fill_line = mut_z.pop(fill_i)
        mut_z.insert(write_i, fill_line)   # the fill's line now sits AFTER the write call
        ok8, detail = gate_before_pattern(mut_z, 0, len(mut_z), PASTE_FILL_MOVES_RE,
                                           PASTE_WRITE_CALL_RE, "gb_paste_hook (MUT Z)")
        check(not ok8, f"MUT Z (gb_paste_fill_moves( moved after gb_paste_write() should "
                        f"have been caught but was not: {detail}")
        print(f"  MUT Z demonstration -- gb_paste_fill_moves( line moved after "
              f"gb_paste_write(: {detail}")

    # MUT AD (BACKLOG #168a review D2, check (ad)'s own demonstration): revert
    # drop_held_up's scan guard back to the reviewer's original bare
    # `if (!pdna_bank_serial_trusted()) {` -- on a copy -- and assert
    # up_scan_latch_facts() catches the missing `s_up_scan_done` latch.
    s, e = extract_function(box_lines, r"^static uint8_t\* __attribute__\(\(noinline\)\) drop_held_up\(")
    body = box_lines[s:e]
    guard_i = first_match_line(body, 0, len(body), UP_SCAN_GUARD_RE)
    check(guard_i is not None, "MUT AD: could not locate drop_held_up's scan guard line "
                                "in the real source -- fix this test")
    if guard_i is not None:
        mut_ad = list(body)
        mut_ad[guard_i] = "  if (!pdna_bank_serial_trusted()) {"
        ok9, detail = up_scan_latch_facts(mut_ad, 0, len(mut_ad))
        check(not ok9, f"MUT AD (scan guard reverted to the bare "
                        f"`!pdna_bank_serial_trusted()` form) should have been caught but "
                        f"was not: {detail}")
        print(f"  MUT AD demonstration -- drop_held_up's scan guard reverted to the bare "
              f"`if (!pdna_bank_serial_trusted())` form: {detail}")

    # MUT AG (BACKLOG #210): delete the `s_busy_reading();` line on a copy -- (ag) must
    # fail to find any busy call before the fill's cold ROM scan.
    busy_i = first_match_line(hook_body, 0, len(hook_body), BUSY_READING_RE)
    check(busy_i is not None, "MUT AG: could not locate the real s_busy_reading() call "
                               "in gb_paste_hook -- fix this test")
    if busy_i is not None:
        mut_ag = [ln for i, ln in enumerate(hook_body) if i != busy_i]
        ok12, detail = gate_before_pattern(mut_ag, 0, len(mut_ag), BUSY_READING_RE,
                                           PASTE_FILL_MOVES_RE, "gb_paste_hook (MUT AG)")
        check(not ok12, f"MUT AG (s_busy_reading() deleted) should have been caught but "
                        f"was not: {detail}")
        print(f"  MUT AG demonstration -- s_busy_reading() call deleted from "
              f"gb_paste_hook: {detail}")

    # BACKLOG #212: gb_bank_down_bridge's own per-slot move rule -- MUT AJ/MUT AH/MUT AI.
    sbr, ebr = extract_function(gen12_lines, r"^BankDownResult gb_bank_down_bridge\(")
    bridge_body = gen12_lines[sbr:ebr]

    # MUT AJ (review D5): delete the bridge's own `s_busy_reading();` line on a copy --
    # (aj) must fail to find any busy call before the fill's cold ROM scan.
    busy_i2 = first_match_line(bridge_body, 0, len(bridge_body), BUSY_READING_RE)
    check(busy_i2 is not None, "MUT AJ: could not locate the real s_busy_reading() call "
                                "in gb_bank_down_bridge -- fix this test")
    if busy_i2 is not None:
        mut_aj = [ln for i, ln in enumerate(bridge_body) if i != busy_i2]
        ok13, detail = gate_before_pattern(mut_aj, 0, len(mut_aj), BUSY_READING_RE,
                                           PASTE_FILL_MOVES_RE, "gb_bank_down_bridge (MUT AJ)")
        check(not ok13, f"MUT AJ (s_busy_reading() deleted) should have been caught but "
                        f"was not: {detail}")
        print(f"  MUT AJ demonstration -- s_busy_reading() call deleted from "
              f"gb_bank_down_bridge: {detail}")

    # MUT AH: move the `gb_paste_fill_moves(` call line to AFTER
    # `gb_paste_legal_screen_ex(` on a copy -- (ah) must fail: the modal would list
    # swap rows for fills that have not happened yet.
    fill_i = first_match_line(bridge_body, 0, len(bridge_body), PASTE_FILL_MOVES_RE)
    modal_i = first_match_line(bridge_body, 0, len(bridge_body), LEGAL_SCREEN_EX_RE)
    check(fill_i is not None and modal_i is not None and fill_i < modal_i,
          "MUT AH: could not locate gb_paste_fill_moves( before gb_paste_legal_screen_ex( "
          "in the real source -- fix this test")
    if fill_i is not None and modal_i is not None and fill_i < modal_i:
        mut_ah = list(bridge_body)
        fill_line = mut_ah.pop(fill_i)
        mut_ah.insert(modal_i, fill_line)
        ok10, detail = gate_before_pattern(mut_ah, 0, len(mut_ah), PASTE_FILL_MOVES_RE,
                                           LEGAL_SCREEN_EX_RE, "gb_bank_down_bridge (MUT AH)")
        check(not ok10, f"MUT AH (gb_paste_fill_moves( moved after gb_paste_legal_screen_ex() "
                         f"should have been caught but was not: {detail}")
        print(f"  MUT AH demonstration -- gb_paste_fill_moves( line moved after "
              f"gb_paste_legal_screen_ex( in gb_bank_down_bridge: {detail}")

    # MUT AI: move the `gb_paste_legal_screen_ex(` call line to AFTER `gbs_insert(`
    # on a copy -- (ai) must fail: a CANCEL choice would arrive too late to stop the
    # box write.
    modal_i2 = first_match_line(bridge_body, 0, len(bridge_body), LEGAL_SCREEN_EX_RE)
    write_i = first_match_line(bridge_body, 0, len(bridge_body), GBS_INSERT_CALL_RE)
    check(modal_i2 is not None and write_i is not None and modal_i2 < write_i,
          "MUT AI: could not locate gb_paste_legal_screen_ex( before gbs_insert( in "
          "the real source -- fix this test")
    if modal_i2 is not None and write_i is not None and modal_i2 < write_i:
        mut_ai = list(bridge_body)
        modal_line = mut_ai.pop(modal_i2)
        mut_ai.insert(write_i, modal_line)
        ok11, detail = gate_before_pattern(mut_ai, 0, len(mut_ai), LEGAL_SCREEN_EX_RE,
                                           GBS_INSERT_CALL_RE, "gb_bank_down_bridge (MUT AI)")
        check(not ok11, f"MUT AI (gb_paste_legal_screen_ex( moved after gbs_insert() should "
                         f"have been caught but was not: {detail}")
        print(f"  MUT AI demonstration -- gb_paste_legal_screen_ex( line moved after "
              f"gbs_insert( in gb_bank_down_bridge: {detail}")

def self_test_al_mutation(main_lines: list[str]) -> None:
    """BACKLOG #213 (al): prove invalidate_call_facts() actually has teeth. Takes
    app_xfer_promote's real (comment-stripped) body and deletes its one
    app_xv_cache_invalidate( line on an in-memory copy -- the exact shape of the bug
    this check exists to catch (a write site whose invalidation call was never added,
    or got deleted in a later edit) -- and asserts invalidate_call_facts()'s own
    any(...) scan reports failure on that copy."""
    s, e = extract_function(main_lines, r"^bool __attribute__\(\(noinline\)\) app_xfer_promote\(void\) \{")
    body = main_lines[s:e]
    mut_al = [ln for ln in body if not INVALIDATE_RE.search(ln)]
    check(any(INVALIDATE_RE.search(ln) for ln in body),
          "MUT AL: app_xfer_promote's real body has no app_xv_cache_invalidate( call to "
          "delete -- fix this test, the real source regressed")
    still_present = any(INVALIDATE_RE.search(ln) for ln in mut_al)
    check(not still_present,
          "MUT AL (app_xfer_promote's invalidate call deleted) should have been caught "
          "but was not -- the mutated copy still reports a call present")
    print("  MUT AL demonstration -- app_xfer_promote's app_xv_cache_invalidate( call "
          f"deleted: {'still (wrongly) found' if still_present else 'correctly absent, check would fail'}")


# ---- (m) 2026-09-16 (merged-tree shot lane): the DOWN dispatch's `occupied` refusal is scoped to
# the GEN3 arm. An unscoped `&& !occupied` on the dispatch condition made the EXACT arm refuse
# every drop on an occupied Game Boy cell (a GB list appends; the cursor cell is irrelevant) and
# put the party-full offer out of reach. Comment-stripped; self-mutation below.
GEN3_SCOPED_RE = re.compile(r"!\(arm == XG_DOWN_ARM_GEN3 && occupied\)")
BARE_OCC_RE    = re.compile(r"&&\s*!occupied\b")

def down_dispatch_occupied_scoped(lines):
    s, e = extract_function(lines, r"^static uint8_t\* drop_held\(")
    body = lines[s:e]
    disp = first_match_line(body, 0, len(body), re.compile(r"bank_down_dispatch\(src, box, cur, s_held"))
    if disp is None: return False, "drop_held: no bank_down_dispatch call"
    window = body[max(0, disp - 8):disp]
    if any(BARE_OCC_RE.search(l) for l in window):
        return False, "drop_held: the DOWN dispatch condition carries a bare `&& !occupied` (must be scoped to the GEN3 arm)"
    if not any(GEN3_SCOPED_RE.search(l) for l in window):
        return False, "drop_held: the DOWN dispatch condition lacks `!(arm == XG_DOWN_ARM_GEN3 && occupied)`"
    return True, "ok"

def _run_check_m():
    lines = strip_comments(BOX_C.read_text()).split("\n") if "strip_comments" in globals() else BOX_C.read_text().split("\n")
    ok, d = down_dispatch_occupied_scoped(lines); check(ok, d)
    # MUT N: un-scope it back to the union's bare form -- must be caught
    s, e = extract_function(lines, r"^static uint8_t\* drop_held\(")
    mut = list(lines)
    for i in range(s, e):
        if GEN3_SCOPED_RE.search(mut[i]):
            mut[i] = GEN3_SCOPED_RE.sub("!occupied", mut[i]); break
    ok2, d2 = down_dispatch_occupied_scoped(mut)
    check(not ok2, "MUT N (bare !occupied restored) should have been caught but was not")
    print(f"  MUT N demonstration -- `!occupied` un-scoped from the GEN3 arm: {d2}")

if __name__ == "__main__":
    _run_check_m()
    sys.exit(main())
