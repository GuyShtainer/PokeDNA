#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_xfer_reconcile_sites_test.py -- structural guard for BACKLOG #150 S150-11's
decision 11 (#176 recovery) and the write-ordering the Bank-open reconcile depends on
(decision 4/8a/9, §3.2). Pure-text checks against the shipped source, no mgba, no
build -- same posture as host_gb_write_gate_test.py, applied to source/pdna_main.c and
source/pdna_bank.c instead of source/pdna_gen12.c.

Seven checks, each with an in-memory mutation that must turn it red:

  (g) app_xfer_save_now()'s app_commit_pc() FAILURE branch (the `else` this slice
      adds) calls app_xfer_pending_drop() (review D2 -- NOT app_xfer_pending_undo(),
      which would remove the ledger's PENDING entry even for a landed-but-unconfirmed
      write, an uncollectable duplicate) -- without clearing the RAM key at all,
      BACKLOG #176 reproduces: a failed save leaves g_xd_key set for the rest of the
      boot and every later native->Gen-3 drop refuses with SAVE FIRST. BACKLOG #175
      (S150-8d) D14 factored this whole if/else out of flush_on_exit() into
      app_xfer_save_now(); this check now also confirms flush_on_exit() still calls
      the helper (a behaviour-identical refactor, not a removal).

  (h) every `pdna_bank_clear_deletions();` call site in source/pdna_main.c is within
      2 lines of `app_xfer_pending_drop();`/`app_xfer_pending_undo();` -- a fresh
      session/save-switch must never inherit a PREVIOUS save's pending transfer, and
      a CANCELLED flush (flush_on_exit's DECLINE branch) must not leave one behind
      either. NOTE: the brief's own citation says there are four LOAD sites; this
      tree has SIX total occurrences -- four decision-11(ii)-tagged load sites, a
      genuine fifth "a new save is now live" site at the tail of the boot sequence
      the brief's DRIFT section did not enumerate, and a sixth, pre-#150-S150-11 site
      inside flush_on_exit's own DECLINE branch ("move cancelled -- keep the Bank
      originals", already paired with app_xfer_pending_undo() one line above) --
      flagged in the S150-11 delivery report per the brief's own STOP-LICENCE
      ("a fifth load site... report, do not silently gate"); this check's regex is
      intentionally broad (every occurrence, not just load sites) so a future site
      of either kind is caught the same way.

  (i) app_xfer_reconcile_bank_open()'s body has app_can_edit( AND app_gen3_pc_live(
      strictly BEFORE any f_opendir(/sf_read_full(/log_line( -- G-F2: a Game Boy
      session's Bank visit must perform zero ledger reads, zero box reads, zero log
      lines.

  (j) pdna_bank_put_cell()'s body has app_can_edit( strictly before memcpy( and
      box_save_or_keep_dirty( -- hard rule 4 (writes are Omega-only) applied to the
      RESTORE TO BANK write path.

  (k) xfer_reconcile_apply_bank_open()'s body never calls sf_write_verified( or
      f_unlink( at all (decision 8a: REMOVE DUPLICATE leaves the entry CLAIMED,
      no ledger rewrite) -- the full §3.2 destination-before-ledger ORDER check (the
      brief's xfer_reconcile_apply, covering RESTORE/DELETE/REKEY) applies once the
      TRANSFERS screen (decision 13/14) lands; this check pins what this lane's
      SCOPED apply function actually does today, so a later commit that adds a
      sidecar rewrite to it without re-deriving the ordering is caught.

  (l) BACKLOG #150 S150-11 step 5 -- the TRANSFERS screen's own xfer_reconcile_apply()
      (decision 8/9, §3.2 in full): every destination write --
      pdna_bank_clear_slots(/gb_reconcile_release(/pdna_bank_put_cell( -- appears
      strictly BEFORE the first sf_write_verified(/f_unlink( in the function body.
      A power cut between a landed destination write and its file's rewrite must
      leave a duplicate or an entry, never a loss; writing the ledger first would
      let it drop a copy that was never actually moved/removed/restored.

  (m) xfer_reconcile_apply()'s body has app_can_edit( strictly before its first
      destination/ledger write (hard rule 4 applied to the TRANSFERS screen's own
      APPLY -- review D5; a Game Boy session or read-only cart must refuse, not
      silently write).

Run directly:

    python3 tests/host_xfer_reconcile_sites_test.py

Registered in tests/run_host_tests.py's PY_TESTS list.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN_SRC = ROOT / "source" / "pdna_main.c"
BANK_SRC = ROOT / "source" / "pdna_bank.c"


def strip_comments(text: str) -> str:
    """Blank out /* ... */ and // comments but keep every newline, so a line number
    reported here still points at the real source line, and a comment mentioning a
    gate/call by name in prose cannot satisfy a check that requires the CODE to call
    it (same discipline host_gb_write_gate_test.py's own strip_comments documents)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text[i:i + 2] == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text[i:i + 2] == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def extract_function_body(text: str, func_name: str) -> str:
    """Same brace-depth walk as host_gb_write_gate_test.py's own helper."""
    m = re.search(r"^\w[\w \*]*\b" + re.escape(func_name) + r"\s*\([^;]*?\)\s*\{",
                  text, re.MULTILINE)
    if not m:
        m = re.search(r"\b" + re.escape(func_name) + r"\s*\([^;{]*\)\s*\{", text)
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


def check_g_flush_on_exit_undo(text: str) -> list[str]:
    # BACKLOG #175 (S150-8d) D14: flush_on_exit()'s own `if (app_commit_pc())
    # {...} else {...}` shape (the #176 posture this check pins) was factored out
    # into app_xfer_save_now() -- flush_on_exit() now just calls it. Check the
    # helper's own body for the shape, and separately confirm flush_on_exit() still
    # reaches it (a behaviour-identical refactor, not a removal).
    exit_body = extract_function_body(text, "flush_on_exit")
    if not exit_body:
        return ["flush_on_exit() not found in source/pdna_main.c"]
    if "app_xfer_save_now(" not in strip_comments(exit_body):
        return ["flush_on_exit(): no app_xfer_save_now( call found -- BACKLOG #175 "
                "D14's refactor moved the #176 posture there; flush_on_exit() must "
                "still call it"]
    body = extract_function_body(text, "app_xfer_save_now")
    if not body:
        return ["app_xfer_save_now() not found in source/pdna_main.c"]
    stripped = strip_comments(body)
    m = re.search(r"if\s*\(\s*app_commit_pc\s*\(\s*\)\s*\)\s*\{", stripped)
    if not m:
        return ["app_xfer_save_now(): no `if (app_commit_pc())` found"]
    # the else branch: from the matching close-brace of the if-block to the next
    # `else {` ... matching close-brace.
    depth = 1
    i = m.end()
    while i < len(stripped) and depth > 0:
        if stripped[i] == "{":
            depth += 1
        elif stripped[i] == "}":
            depth -= 1
        i += 1
    rest = stripped[i:]
    em = re.match(r"\s*else\s*\{", rest)
    if not em:
        return ["app_xfer_save_now(): app_commit_pc()'s if-block has no `else` -- BACKLOG #176 "
                "regression: a failed commit never clears the pending transfer"]
    depth = 1
    j = em.end()
    while j < len(rest) and depth > 0:
        if rest[j] == "{":
            depth += 1
        elif rest[j] == "}":
            depth -= 1
        j += 1
    else_body = rest[em.end():j]
    out = []
    # review D2: app_commit_pc() returning false does NOT mean nothing landed
    # (app_save_finalize()'s SF_WHERE_TARGET branch returns false for a write that
    # IS on the card, only unconfirmed) -- app_xfer_pending_undo() would then REMOVE
    # the ledger's PENDING entry over a copy that actually landed, an uncollectable
    # duplicate. Only the RAM key is cleared here: app_xfer_pending_drop(), not undo.
    if "app_xfer_pending_drop(" not in else_body:
        out.append("app_xfer_save_now(): the app_commit_pc() FAILURE branch does not call "
                    "app_xfer_pending_drop( -- BACKLOG #176/review D2 (a failed-or-"
                    "unconfirmed save must not remove the ledger's PENDING entry)")
    if "PDNA_XFER_NOTSAVED_TITLE" not in else_body:
        out.append("app_xfer_save_now(): the app_commit_pc() FAILURE branch does not show "
                    "PDNA_XFER_NOTSAVED_TITLE (decision 11(i))")
    return out


CLEAR_DEL_RE = re.compile(r"pdna_bank_clear_deletions\s*\(\s*\)\s*;")
# app_xfer_pending_undo() ALSO clears g_xd_key (its own last statement is
# app_xfer_pending_drop()) -- flush_on_exit's DECLINE branch already calls it right
# before its own pdna_bank_clear_deletions(), so either call satisfies "this save's
# pending transfer does not survive past this point".
PENDING_DROP_RE = re.compile(r"app_xfer_pending_drop\s*\(\s*\)\s*;|app_xfer_pending_undo\s*\(\s*\)\s*;")


def check_h_load_sites(text: str) -> tuple[list[str], int]:
    """Returns (violations, sites_found) -- sites_found is reported by main() so a
    drift in the SITE COUNT itself (not just a missing drop) is visible, not silent."""
    stripped_lines = strip_comments(text).split("\n")
    sites = [i for i, l in enumerate(stripped_lines) if CLEAR_DEL_RE.search(l)]
    violations = []
    for i in sites:
        window = stripped_lines[max(0, i - 2):i] + stripped_lines[i + 1:i + 3]
        if not any(PENDING_DROP_RE.search(l) for l in window):
            violations.append(f"pdna_main.c:{i + 1}: `pdna_bank_clear_deletions();` has no "
                              f"`app_xfer_pending_drop();`/`app_xfer_pending_undo();` within 2 "
                              f"lines -- decision 11(ii)")
    return violations, len(sites)


def check_i_bank_open_gate(text: str) -> list[str]:
    body = extract_function_body(text, "app_xfer_reconcile_bank_open")
    if not body:
        return ["app_xfer_reconcile_bank_open() not found in source/pdna_main.c"]
    stripped = strip_comments(body)
    can_edit_pos = [m.start() for m in re.finditer(r"app_can_edit\s*\(", stripped)]
    pc_live_pos = [m.start() for m in re.finditer(r"app_gen3_pc_live\s*\(", stripped)]
    guard_pos = [m.start() for m in re.finditer(r"f_opendir\s*\(|sf_read_full\s*\(|log_line\s*\(", stripped)]
    out = []
    if not can_edit_pos:
        out.append("app_xfer_reconcile_bank_open(): never calls app_can_edit( (G-F2)")
    if not pc_live_pos:
        out.append("app_xfer_reconcile_bank_open(): never calls app_gen3_pc_live( (G-F2)")
    if can_edit_pos and pc_live_pos and guard_pos:
        first_guard = min(guard_pos)
        if min(can_edit_pos) >= first_guard or min(pc_live_pos) >= first_guard:
            out.append("app_xfer_reconcile_bank_open(): app_can_edit(/app_gen3_pc_live( must "
                        "both appear before the first f_opendir(/sf_read_full(/log_line( (G-F2)")
    return out


def check_j_put_cell_gate(text: str) -> list[str]:
    body = extract_function_body(text, "pdna_bank_put_cell")
    if not body:
        return ["pdna_bank_put_cell() not found in source/pdna_bank.c"]
    stripped = strip_comments(body)
    can_edit_pos = [m.start() for m in re.finditer(r"app_can_edit\s*\(", stripped)]
    guard_pos = [m.start() for m in re.finditer(r"memcpy\s*\(|box_save_or_keep_dirty\s*\(", stripped)]
    out = []
    if not can_edit_pos:
        out.append("pdna_bank_put_cell(): never calls app_can_edit( (hard rule 4)")
    elif guard_pos and min(can_edit_pos) >= min(guard_pos):
        out.append("pdna_bank_put_cell(): app_can_edit( must appear before the first "
                    "memcpy(/box_save_or_keep_dirty( (hard rule 4)")
    return out


def check_k_apply_no_ledger_write(text: str) -> list[str]:
    body = extract_function_body(text, "xfer_reconcile_apply_bank_open")
    if not body:
        return ["xfer_reconcile_apply_bank_open() not found in source/pdna_main.c"]
    stripped = strip_comments(body)
    out = []
    if "pdna_bank_clear_slots(" not in stripped:
        out.append("xfer_reconcile_apply_bank_open(): never calls pdna_bank_clear_slots( "
                    "(decision 8a)")
    if "sf_write_verified(" in stripped or "f_unlink(" in stripped:
        out.append("xfer_reconcile_apply_bank_open(): calls sf_write_verified(/f_unlink( -- "
                    "decision 8a says REMOVE DUPLICATE never rewrites the ledger (the entry "
                    "stays CLAIMED); if a later commit adds one, its ORDER relative to "
                    "pdna_bank_clear_slots( must be re-derived (§3.2), not assumed safe")
    return out


def check_l_screen_apply_order(text: str) -> list[str]:
    body = extract_function_body(text, "xfer_reconcile_apply")
    if not body:
        return ["xfer_reconcile_apply() not found in source/pdna_main.c"]
    stripped = strip_comments(body)
    dest_pos = [m.start() for m in re.finditer(
        r"pdna_bank_clear_slots\s*\(|gb_reconcile_release\s*\(|pdna_bank_put_cell\s*\(",
        stripped)]
    ledger_pos = [m.start() for m in re.finditer(
        r"sf_write_verified\s*\(|f_unlink\s*\(", stripped)]
    out = []
    if not dest_pos:
        out.append("xfer_reconcile_apply(): never calls pdna_bank_clear_slots(/"
                    "gb_reconcile_release(/pdna_bank_put_cell( (decision 8a/8b/8c)")
    if not ledger_pos:
        out.append("xfer_reconcile_apply(): never calls sf_write_verified(/f_unlink( "
                    "(decision 9's per-file rewrite)")
    # max(dest_pos), not min(dest_pos): comparing only the FIRST destination write
    # (min) misses a LATER destination write that landed after the first ledger
    # rewrite -- every destination write must precede every ledger write, so the
    # LAST destination write is the one that must still come before the ledger
    # (BACKLOG #150 S150-11 review D3).
    if dest_pos and ledger_pos and max(dest_pos) >= min(ledger_pos):
        out.append("xfer_reconcile_apply(): a ledger write (sf_write_verified(/f_unlink() "
                    "appears before every destination write -- §3.2 order violated")
    return out


def check_m_apply_gate(text: str) -> list[str]:
    """(m) xfer_reconcile_apply()'s body calls app_can_edit( strictly before its
    first destination/ledger write -- hard rule 4 (writes are Omega-only) applied
    to the TRANSFERS screen's own APPLY, review D5."""
    body = extract_function_body(text, "xfer_reconcile_apply")
    if not body:
        return ["xfer_reconcile_apply() not found in source/pdna_main.c"]
    stripped = strip_comments(body)
    can_edit_pos = [m.start() for m in re.finditer(r"app_can_edit\s*\(", stripped)]
    guard_pos = [m.start() for m in re.finditer(
        r"pdna_bank_clear_slots\s*\(|gb_reconcile_release\s*\(|pdna_bank_put_cell\s*\(|"
        r"sf_write_verified\s*\(|f_unlink\s*\(",
        stripped)]
    out = []
    if not can_edit_pos:
        out.append("xfer_reconcile_apply(): never calls app_can_edit( (hard rule 4, review D5)")
    elif guard_pos and min(can_edit_pos) >= min(guard_pos):
        out.append("xfer_reconcile_apply(): app_can_edit( must appear before the first "
                    "destination/ledger write (hard rule 4, review D5)")
    return out


def check_n_bank_g3_wiring(text: str) -> list[str]:
    """(n) BACKLOG #283(c) / #280 step 4: the #270 parked-copy signal is wired END TO END.
    xfer_reconcile_bank_phase2() must count parked Gen-3 copies with xrc_bank_g3_match(recs,
    h->file_key) and xfer_reconcile_classify_all() must hand the count to the classifier
    (`in.bank_g3_matches = h->bank_g3_matches;`) -- unwired, a Gen-3 copy parked in the Bank
    classifies LOST with RESTORE (= a clone) / DELETE (= the way home lost)."""
    out = []
    phase2 = strip_comments(extract_function_body(text, "xfer_reconcile_bank_phase2"))
    if not phase2:
        out.append("xfer_reconcile_bank_phase2() not found in source/pdna_main.c")
    elif not re.search(r"xrc_bank_g3_match\s*\(\s*recs\s*,\s*h->file_key\s*\)", phase2):
        out.append("xfer_reconcile_bank_phase2(): no xrc_bank_g3_match(recs, h->file_key) call")
    classify = strip_comments(extract_function_body(text, "xfer_reconcile_classify_all"))
    if not classify:
        out.append("xfer_reconcile_classify_all() not found in source/pdna_main.c")
    elif "in.bank_g3_matches = h->bank_g3_matches;" not in classify:
        out.append("xfer_reconcile_classify_all(): `in.bank_g3_matches = h->bank_g3_matches;` missing")
    return out


def check_o_g3home_not_listed(text: str) -> list[str]:
    """(o) #280 step 4: the reconcile WALK lists NATIVE_HOME entries only -- a G3_HOME entry (the
    ledger record of a Gen-3 original whose Game Boy copy is parked in the Bank as a native cell)
    never becomes a TRANSFERS row, so no RESTORE (clone) / DELETE (loses the way home) can be
    offered for it. RED if the skip is removed: someone must then re-derive that row's actions."""
    body = strip_comments(extract_function_body(text, "xfer_reconcile_walk"))
    if not body:
        return ["xfer_reconcile_walk() not found in source/pdna_main.c"]
    if not re.search(r"e2\.kind\s*!=\s*XR_KIND_NATIVE_HOME\s*\)\s*continue", body):
        return ["xfer_reconcile_walk(): the `e2.kind != XR_KIND_NATIVE_HOME) continue` skip is gone -- "
                "G3_HOME entries would reach the classifier/TRANSFERS"]
    return []


def check_p_readonly_draft_walk(text: str) -> list[str]:
    """(p) #414: the TRANSFERS walk admits an orphan <key>.pds.tmp ONLY on a read-only card (the writable card
    heals it first), skips it when the primary is present, judges it by the shared #415 predicate, and the row text
    goes through xrc_text_of. RED if any of the four is dropped."""
    body = strip_comments(extract_function_body(text, "xfer_reconcile_walk"))
    out = []
    if not body:
        return ["xfer_reconcile_walk() not found in source/pdna_main.c"]
    if not re.search(r"ro_walk\s*=\s*!app_can_edit\(\)", body):
        out.append("xfer_reconcile_walk(): ro_walk = !app_can_edit() gate missing (#414)")
    # #419: the admit decision is the pure xrc_walk_admit (host-proven, ADMIT-1: W2 prim/primary, name_key, allow_draft gate);
    # the walk must hand it ro_walk as allow_draft and act on its DRAFT verdict.
    if not re.search(r"xrc_walk_admit\(\s*fi\.fname\s*,[^;]*,\s*ro_walk\b[^,;]*,\s*&name_key\s*,\s*prim\s*\)", body):
        out.append("xfer_reconcile_walk(): the admit is not xrc_walk_admit(..., ro_walk, &name_key, prim) (#414/#419)")
    if not re.search(r"case\s+XRC_ADMIT_DRAFT\s*:", body):
        out.append("xfer_reconcile_walk(): no XRC_ADMIT_DRAFT case (#414/#419)")
    # review-zg2: pin the ARGUMENT -- `xr_path_for_name(rb->path, fi.fname)` (the walk-level W2: f_stat the .tmp itself, so
    # primary-wins is always true and the draft feature is dead) passed the old `[^)]*` form.
    if not re.search(r"if\s*\(\s*xr_path_for_name\(\s*rb->path\s*,\s*prim\s*\)\s*\)\s*continue", body) or \
       not re.search(r"char\*\s*prim\s*=\s*rb->names\[\s*rb->nfiles\s*\]\s*;", body):
        out.append("xfer_reconcile_walk(): primary-wins skip must f_stat prim (= rb->names[rb->nfiles]), not the .tmp name (#414/#419 W2)")
    if not re.search(r"draft\s*&&\s*!xrc_draft_accept\(\s*rb->sidecar\s*,\s*len\s*,\s*name_key\s*\)", body):
        out.append("xfer_reconcile_walk(): xrc_draft_accept(rb->sidecar, len, name_key) is not applied to the draft (#414/#415/#419)")
    vt = strip_comments(extract_function_body(text, "xrc_visible_text"))
    if vt.count("xrc_text_of(") != 2 or "xrc_row_text(" in vt:
        out.append("xrc_visible_text(): the draft label (xrc_text_of) is not on the row text path (#414)")
    return out


def check_q_legacy_sidecar_pass(text: str) -> list[str]:
    """(#418 n/o/p/q) the TRANSFERS walk's second pass over the legacy /sidecar dir: (n) gated on !xr_migrated(); (o) never
    admits a .tmp draft (allow_draft is pass 0 only -- xr_open cannot read a /sidecar .tmp); (p) the dedupe strcmp over
    rb->names precedes the accept (rb->nfiles++), primary = the xfer copy wins; (q) `examined` is never reset between passes."""
    body = strip_comments(extract_function_body(text, "xfer_reconcile_walk"))
    out = []
    if not body:
        return ["xfer_reconcile_walk() not found in source/pdna_main.c"]
    if not re.search(r"legacy_ok\s*=\s*!xr_migrated\(\)", body) or \
       not re.search(r"if\s*\(\s*pass\s*==\s*1\s*\)\s*\{\s*if\s*\(\s*!legacy_ok\s*\)\s*break;", body):
        out.append("#418(n): pass 2 over PDNA_SIDECAR_DIR is not gated on !xr_migrated()")
    if "PDNA_SIDECAR_DIR" not in body:
        out.append("#418(n): xfer_reconcile_walk never opens PDNA_SIDECAR_DIR")
    if not re.search(r"xrc_walk_admit\([^;]*,\s*ro_walk\s*&&\s*pass\s*==\s*0\s*,", body):
        out.append("#418(o): the xrc_walk_admit call does not restrict allow_draft to pass 0 (ro_walk && pass == 0)")
    dm = re.search(r"if\s*\(\s*pass\s*==\s*1\s*\)\s*\{[^}]*?for\s*\(\s*int\s+k\s*=\s*0\s*;\s*k\s*<\s*rb->nfiles\s*;\s*k\+\+\s*\)\s*"
                   r"if\s*\(\s*xrc_name_eq_ci\(\s*rb->names\[\s*k\s*\]\s*,\s*fi\.fname\s*\)\s*\)", body)
    am = body.find("rb->nfiles++")
    if not dm or am < 0 or dm.start() > am or not re.search(r"if\s*\(\s*dup\s*\)\s*continue", body):
        out.append("#418(p): the dedupe xrc_name_eq_ci(rb->names[k], fi.fname) + continue must precede the accept (rb->nfiles++)")
    # review-zg2: the declaration itself must sit BEFORE the pass loop -- `int examined = 0;` moved inside the loop resets it
    # per pass (the S150-6 F6 regression) and passed the old replace()-based check.
    dq, fp = body.find("int examined = 0;"), body.find("for (int pass = 0;")
    if dq < 0 or fp < 0 or dq > fp or len(re.findall(r"\bexamined\s*=\s*0\b", body)) != 1:
        out.append("#418(q): `examined` is reset between passes (the bound must be global across both)")
    # review-zg2 (r): an absent /PokeDNA/xfer must FALL THROUGH to the /sidecar pass (an unmigrated card with only /sidecar),
    # never return early as the pre-#418 walk did.
    if not re.search(r"if\s*\(\s*f_opendir\(&dir,\s*pass\s*==\s*0\s*\?\s*PDNA_XFER_DIR\s*:\s*PDNA_SIDECAR_DIR\)\s*!=\s*FR_OK\)\s*continue;", body) \
       or re.search(r"\breturn\b", body):
        out.append("#418(r): an absent xfer dir must fall through to pass 1 (continue), and the walk must not return early")
    # #421: legacy_n++ must be after gbsc_count passes (not before sf_read_full)
    cn = re.search(r"if\s*\(\s*count\s*<\s*0\s*\)\s*\{[^}]*continue;", body)
    ln = re.search(r"if\s*\(\s*pass\s*==\s*1\s*\)\s*legacy_n\+\+;", body)
    if not cn or not ln or cn.end() > ln.start():
        out.append("#421: legacy_n++ must be after the gbsc_count(count < 0) check")
    return out


def run_all(main_text: str, bank_text: str) -> tuple[list[str], int]:
    violations = list(check_g_flush_on_exit_undo(main_text))
    h_violations, h_sites = check_h_load_sites(main_text)
    violations += h_violations
    violations += check_i_bank_open_gate(main_text)
    violations += check_j_put_cell_gate(bank_text)
    violations += check_k_apply_no_ledger_write(main_text)
    violations += check_l_screen_apply_order(main_text)
    violations += check_m_apply_gate(main_text)
    violations += check_n_bank_g3_wiring(main_text)
    violations += check_o_g3home_not_listed(main_text)
    violations += check_p_readonly_draft_walk(main_text)
    violations += check_q_legacy_sidecar_pass(main_text)
    return violations, h_sites


def main() -> int:
    if not MAIN_SRC.exists() or not BANK_SRC.exists():
        print("SKIP (source/pdna_main.c or source/pdna_bank.c not found)")
        return 0

    main_text = MAIN_SRC.read_text()
    bank_text = BANK_SRC.read_text()

    violations, h_sites = run_all(main_text, bank_text)
    print(f"(h) pdna_bank_clear_deletions() load sites found: {h_sites} "
          f"(the brief's own citation says 4 -- this tree has {h_sites}, see the "
          f"module docstring)")
    if violations:
        print("FAIL -- shipped source has an S150-11 #176/site gap:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: (g) flush_on_exit's failure branch drops the pending-transfer key, "
          f"(h) all {h_sites} load sites drop it, (i) the Bank-open gate runs first, "
          "(j) pdna_bank_put_cell gates on app_can_edit, (k) the scoped apply "
          "function never rewrites the ledger, (l) the TRANSFERS screen's own "
          "apply orders every destination write before the first ledger write, "
          "(m) that same apply gates on app_can_edit before any write")

    # --- self-mutation proofs -----------------------------------------------------
    fails = 0

    # (g): delete the drop call from the else branch. BACKLOG #175 (S150-8d) D14
    # moved this from flush_on_exit (6-space indent, 3 nesting levels) into
    # app_xfer_save_now (4-space indent, 2 nesting levels) -- re-anchored, not
    # copied blind.
    mutated = main_text.replace(
        "    if (!app_xfer_pending_is_g3home()) app_xfer_pending_drop();\n"   # #284: guarded by the kind
        "    msg_wait(PDNA_XFER_NOTSAVED_TITLE, UI_WARN, PDNA_XFER_NOTSAVED_L1, PDNA_XFER_NOTSAVED_L2);\n",
        "    msg_wait(PDNA_XFER_NOTSAVED_TITLE, UI_WARN, PDNA_XFER_NOTSAVED_L1, PDNA_XFER_NOTSAVED_L2);\n",
        1)
    if mutated == main_text:
        print("FAIL -- self-mutation (g) target not found verbatim (source drifted)")
        fails += 1
    else:
        v, _ = run_all(mutated, bank_text)
        if not any("app_xfer_pending_drop(" in x for x in v):
            print("FAIL -- self-mutation (g): removing the drop call did NOT turn check (g) red")
            fails += 1
        else:
            print("self-mutation (g): removing the drop call -- correctly caught")

    # (h): delete ONE of THIS SLICE's own app_xfer_pending_drop() sites (tagged with
    # the S150-11 decision 11(ii) comment -- the pre-existing app_xfer_pending_drop()
    # calls inside app_xfer_promote/app_xfer_pending_undo are a different mechanism
    # entirely and must not be the mutation target).
    site_re = re.compile(r"[ \t]*app_xfer_pending_drop\(\);   /\* BACKLOG #150 S150-11 decision 11\(ii\).*\n")
    m = site_re.search(main_text)
    if not m:
        print("FAIL -- self-mutation (h): no tagged decision-11(ii) drop site found to remove")
        fails += 1
    else:
        mutated_h = main_text[:m.start()] + main_text[m.end():]
        v, sites = run_all(mutated_h, bank_text)
        if not any("decision 11(ii)" in x for x in v):
            print("FAIL -- self-mutation (h): removing one tagged drop-site call did NOT turn check (h) red")
            fails += 1
        else:
            print(f"self-mutation (h): removing one of {sites} tagged drop calls -- correctly caught")

    # (i): swap the gate order (guard call before the app_can_edit/app_gen3_pc_live checks).
    target_i = "  if (!app_can_edit()) return;\n  if (!app_gen3_pc_live()) return;\n"
    if target_i not in main_text:
        print("FAIL -- self-mutation (i) target not found verbatim (source drifted)")
        fails += 1
    else:
        mutated_i = main_text.replace(
            target_i,
            "  log_line(\"xfer: reconcile(bank-open): entered\");\n" + target_i,
            1)
        v, _ = run_all(mutated_i, bank_text)
        if not any("app_xfer_reconcile_bank_open" in x for x in v):
            print("FAIL -- self-mutation (i): a log_line before the gate did NOT turn check (i) red")
            fails += 1
        else:
            print("self-mutation (i): a log_line() spliced before the gate -- correctly caught")

    # (j): delete the early app_can_edit( gate entirely, WITHIN pdna_bank_put_cell's
    # own body only (the string appears 3x in pdna_bank.c -- box_save_or_keep_dirty
    # and pdna_bank_prepare_native's own callees have their own, unrelated copies).
    body_j = extract_function_body(bank_text, "pdna_bank_put_cell")
    target_j = "  if (!app_can_edit()) return false;\n"
    if not body_j or target_j not in body_j:
        print("FAIL -- self-mutation (j) target not found verbatim inside pdna_bank_put_cell (source drifted)")
        fails += 1
    else:
        mutated_body_j = body_j.replace(target_j, "", 1)
        mutated_j = bank_text.replace(body_j, mutated_body_j, 1)
        v, _ = run_all(main_text, mutated_j)
        if not any("pdna_bank_put_cell" in x for x in v):
            print("FAIL -- self-mutation (j): removing the early app_can_edit( gate did NOT turn check (j) red")
            fails += 1
        else:
            print("self-mutation (j): the early app_can_edit( gate removed -- correctly caught")

    # (k): splice an sf_write_verified( call into the apply function.
    body_k = extract_function_body(main_text, "xfer_reconcile_apply_bank_open")
    if not body_k or "return removed;" not in body_k:
        print("FAIL -- self-mutation (k) target not found verbatim (source drifted)")
        fails += 1
    else:
        mutated_body = body_k.replace(
            "return removed;",
            "sf_write_verified(rb->path, rb->sidecar, 0); return removed;", 1)
        mutated_k = main_text.replace(body_k, mutated_body, 1)
        v, _ = run_all(mutated_k, bank_text)
        if not any("sf_write_verified" in x for x in v):
            print("FAIL -- self-mutation (k): splicing an sf_write_verified( call did NOT turn check (k) red")
            fails += 1
        else:
            print("self-mutation (k): an sf_write_verified( call spliced into the scoped apply -- correctly caught")

    # (l): splice an sf_write_verified( call to the very TOP of xfer_reconcile_apply(),
    # ahead of every destination write -- proves check (l) actually re-derives the
    # ORDER, not just presence (the way check (k)'s own mutation only proves presence
    # for the bank-open variant).
    target_l = "  bool remove_entry[GB_RECON_MAX_HITS];\n  memset(remove_entry, 0, sizeof remove_entry);\n"
    if target_l not in main_text:
        print("FAIL -- self-mutation (l) target not found verbatim (source drifted)")
        fails += 1
    else:
        mutated_l = main_text.replace(
            target_l,
            target_l + "  sf_write_verified(rb->path, rb->sidecar, 0);\n", 1)
        v, _ = run_all(mutated_l, bank_text)
        if not any("xfer_reconcile_apply(): a ledger write" in x for x in v):
            print("FAIL -- self-mutation (l): an early sf_write_verified( call did NOT turn check (l) red")
            fails += 1
        else:
            print("self-mutation (l): sf_write_verified( spliced ahead of every destination write -- correctly caught")

    # (l)-D3: move block (2) (the per-file rewrite loop, its own ledger write) to
    # directly ABOVE block (1c) (RESTORE TO BANK, a destination write) inside
    # xfer_reconcile_apply() itself -- (1a)/(1b) stay first, so min(dest_pos) is
    # UNCHANGED and a min(dest_pos)-based check would miss this; only comparing
    # max(dest_pos) (the now-late (1c) RESTORE call) against min(ledger_pos) (the
    # now-early block-(2) rewrite) catches the real violation (review D3).
    body_l = extract_function_body(main_text, "xfer_reconcile_apply")
    m_1c = re.search(r"  /\* \(1c\) RESTORE TO BANK", body_l)
    m_2 = re.search(r"  /\* \(2\) one verified rewrite", body_l)
    m_3 = re.search(r"  /\* \(3\) re-keys", body_l)
    if not (m_1c and m_2 and m_3 and m_1c.start() < m_2.start() < m_3.start()):
        print("FAIL -- self-mutation (l)-D3 markers not found/ordered verbatim (source drifted)")
        fails += 1
    else:
        block_1c = body_l[m_1c.start():m_2.start()]
        block_2 = body_l[m_2.start():m_3.start()]
        swapped_body = body_l[:m_1c.start()] + block_2 + block_1c + body_l[m_3.start():]
        mutated_l2 = main_text.replace(body_l, swapped_body, 1)
        v, _ = run_all(mutated_l2, bank_text)
        if not any("xfer_reconcile_apply(): a ledger write" in x for x in v):
            print("FAIL -- self-mutation (l)-D3: moving block (2) above block (1c) did NOT turn check (l) red")
            fails += 1
        else:
            print("self-mutation (l)-D3: block (2) moved above block (1c) -- correctly caught")

    # (m): delete xfer_reconcile_apply()'s own leading app_can_edit( guard (review D5).
    target_m = ('  if (!app_can_edit()) { log_line("BUG: xfer_reconcile_apply with editing '
                'disabled - refused"); return; }\n')
    if target_m not in main_text:
        print("FAIL -- self-mutation (m) target not found verbatim (source drifted)")
        fails += 1
    else:
        mutated_m = main_text.replace(target_m, "", 1)
        v, _ = run_all(mutated_m, bank_text)
        if not any("xfer_reconcile_apply(): never calls app_can_edit(" in x for x in v):
            print("FAIL -- self-mutation (m): removing the leading app_can_edit( guard did NOT turn check (m) red")
            fails += 1
        else:
            print("self-mutation (m): the leading app_can_edit( guard removed -- correctly caught")

    # (n)/(o) (#283(c) + #280 step 4): each wiring/skip line removed must turn its check red.
    for label, target, needle in (
        ("(n) xrc_bank_g3_match call", "xrc_bank_g3_match(recs, h->file_key)", "xrc_bank_g3_match(recs, h->file_key)"),
        ("(n) in.bank_g3_matches wiring", "in.bank_g3_matches = h->bank_g3_matches;", "in.bank_g3_matches = h->bank_g3_matches;"),
        ("(o) G3_HOME skip", "e2.kind != XR_KIND_NATIVE_HOME) continue;", "the `e2.kind != XR_KIND_NATIVE_HOME) continue` skip"),
    ):
        if target not in main_text:
            print(f"FAIL -- self-mutation {label} target not found verbatim (source drifted)")
            fails += 1
            continue
        repl = "0" if label.startswith("(n) xrc") else ("" if "wiring" in label else "e2.kind != 99) continue;")
        v, _ = run_all(main_text.replace(target, repl, 1), bank_text)
        if not any(("xrc_bank_g3_match" in x or "bank_g3_matches" in x or "skip is gone" in x) for x in v):
            print(f"FAIL -- self-mutation {label}: did NOT turn its check red")
            fails += 1
        else:
            print(f"self-mutation {label}: correctly caught")

    # (p) #414: each of the five wiring pieces removed must turn its check red.
    for label, target, repl in (
        ("(p) ro gate", "const bool ro_walk = !app_can_edit();", "const bool ro_walk = true;"),
        ("(p) admit gate", "(fi.fattrib & AM_DIR) != 0, ro_walk && pass == 0, &name_key, prim)", "(fi.fattrib & AM_DIR) != 0, true, &name_key, prim)"),
        ("(p) primary wins", "if (xr_path_for_name(rb->path, prim)) continue;", ""),
        ("(p) shared predicate", "if (draft && !xrc_draft_accept(", "if (0 && !xrc_draft_accept("),
        ("(p) draft label", "  xrc_text_of(rb, h, c->species", "  xrc_row_text((XrcRowKind)h->row_kind, c->species"),
    ):
        if target not in main_text:
            print(f"FAIL -- self-mutation {label} target not found verbatim (source drifted)")
            fails += 1
            continue
        v, _ = run_all(main_text.replace(target, repl, 1), bank_text)
        if not any("#414" in x for x in v):
            print(f"FAIL -- self-mutation {label}: did NOT turn its check red")
            fails += 1
        else:
            print(f"self-mutation {label}: correctly caught")

    # (#418 n/o/p/q): each pass-2 property removed must turn its own check red.
    for label, target, repl, tag in (
        ("(#418 n) ungated pass 2", "const bool legacy_ok = !xr_migrated();", "const bool legacy_ok = true;", "#418(n)"),
        ("(#418 o) drafts allowed in pass 2", "ro_walk && pass == 0,", "ro_walk,", "#418(o)"),
        ("(#418 p) dedupe dropped", "if (dup) continue;", "", "#418(p)"),
        ("(#418 q) examined reset", "if (pass == 1) { if (!legacy_ok) break; }", "if (pass == 1) { if (!legacy_ok) break; examined = 0; }", "#418(q)"),
        ("(#418 q2) examined declared per pass", "for (int pass = 0; pass < 2; pass++) {", "for (int pass = 0; pass < 2; pass++) { int examined = 0;", "#418(q)"),
        ("(#418 p2) dedupe loop dead", "for (int k = 0; k < rb->nfiles; k++) if (xrc_name_eq_ci", "for (int k = 0; k < 0; k++) if (xrc_name_eq_ci", "#418(p)"),
        ("(#420) dedupe case-sensitive", "xrc_name_eq_ci(rb->names[k], fi.fname)", "(strcmp(rb->names[k], fi.fname) == 0)", "#418(p)"),
        ("#421 legacy_n++ moved before the read", "    gb_recon_path(rb->path, rb->names[fidx]);\n    uint32_t len = 0;\n    if (sf_read_full(rb->path, rb->sidecar, GBSC_FILE_MAX, &len) != SF_OK) {\n      log_line(\"xfer: reconcile: could not read %s\", rb->path);\n      continue;\n    }\n    int count = gbsc_count(rb->sidecar, len);\n    if (count < 0) {\n      log_line(\"xfer: reconcile: %s fails its own crc, skipped\", rb->path);\n      continue;\n    }\n    if (pass == 1) legacy_n++;", "    if (pass == 1) legacy_n++;\n    gb_recon_path(rb->path, rb->names[fidx]);\n    uint32_t len = 0;\n    if (sf_read_full(rb->path, rb->sidecar, GBSC_FILE_MAX, &len) != SF_OK) {\n      log_line(\"xfer: reconcile: could not read %s\", rb->path);\n      continue;\n    }\n    int count = gbsc_count(rb->sidecar, len);\n    if (count < 0) {\n      log_line(\"xfer: reconcile: %s fails its own crc, skipped\", rb->path);\n      continue;\n    }", "#421"),
        ("(#418 p3) dedupe on the wrong pass", "if (pass == 1) {   /* primary wins", "if (pass == 2) {   /* primary wins", "#418(p)"),
        ("(#418 r) absent xfer returns", "PDNA_SIDECAR_DIR) != FR_OK) continue;", "PDNA_SIDECAR_DIR) != FR_OK) return;", "#418(r)"),
        ("(#419 W2) walk f_stats the .tmp", "if (xr_path_for_name(rb->path, prim)) continue;", "if (xr_path_for_name(rb->path, fi.fname)) continue;", "W2"),
    ):
        if target not in main_text:
            print(f"FAIL -- self-mutation {label} target not found verbatim (source drifted)")
            fails += 1
            continue
        v, _ = run_all(main_text.replace(target, repl, 1), bank_text)
        if not any(tag in x for x in v):
            print(f"FAIL -- self-mutation {label}: did NOT turn its check red")
            fails += 1
        else:
            print(f"self-mutation {label}: correctly caught")

    if fails:
        print(f"FAIL -- {fails} self-mutation proof(s) did not fire")
        return 1

    print("ok: all self-mutation proofs fired")
    return 0


if __name__ == "__main__":
    sys.exit(main())
