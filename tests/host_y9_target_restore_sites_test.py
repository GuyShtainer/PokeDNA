#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_y9_target_restore_sites_test.py -- structural pins for BACKLOG #280 (Guy's #270 ruling,
the GB->Bank half): the lift is a pass-through and the original comes home at the TARGET drop.
Pure text checks against the shipped source (no build), the same posture as
host_escape_gate_sites_test.py's Y7 pins. Every fact function is shared by the real check and by
an in-memory MUTATION that must turn it red (the self-test at the bottom).

Slice 2 adds: D. gb_bridge_restore_up (the NATIVE_HOME bridge restore at the other-generation Game Boy
target) -- order, the ALREADY-RESTORED dup rule, the landed-check before the RESTORED mark; E. the
bank_down_convert_gb wrapper; F. the lift is a pass-through (gb_lift_pack / gb_release_up_hook read no
ledger; the retired functions are gone); G. gb_bridge_mark_restored's set_state -> verified write.

Slice 1 (G3_HOME at the Gen-3 target):
  A. gb_g3home_restore_up (source/pdna_gen12.c): copy-cell gate, the write gate and the live-PC
     gate come BEFORE any ledger read; the pending-transfer wall (SAVE NOW?) and the deferred-
     delete room check come BEFORE the merge screen; the merge screen comes before the commit;
     app_xfer_pending_set comes LAST (nothing is recorded for a declined restore); and the
     function WRITES NOTHING to the card (no sf_write_verified/f_unlink/gbsc_remove/gbsc_add).
  B. both Gen-3 wrappers (bank_down_convert_gen3 and _party, source/bank_down_convert.c) call
     gb_g3home_restore_up BEFORE gb_bank_down_gen3, and only rc == 1 short-circuits to CONVERTED.
  C. app_xfer_promote (source/pdna_main.c) consumes a G3_HOME entry (gbsc_remove, then the verified
     rewrite/unlink, then app_xfer_pending_drop) -- and that branch precedes the NATIVE_HOME one.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN12 = ROOT / "source" / "pdna_gen12.c"
CONVERT = ROOT / "source" / "bank_down_convert.c"
MAIN = ROOT / "source" / "pdna_main.c"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip_comments(text: str) -> str:
    """Blank /* */ and // comments, keeping every newline (line numbers stay real)."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i:i + 2] == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text[i:i + 2] == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def function_body(text: str, name: str) -> str:
    """Comment-stripped body of the function `name` (brace walk); '' if absent."""
    text = strip_comments(text)
    m = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.MULTILINE)
    if not m:
        return ""
    depth, j = 1, m.end()
    while j < len(text) and depth > 0:
        depth += (text[j] == "{") - (text[j] == "}")
        j += 1
    return text[m.start():j]


def first(body: str, call: str) -> int:
    m = re.search(r"\b" + re.escape(call) + r"\s*\(", body)
    return m.start() if m else -1


def last(body: str, call: str) -> int:
    ms = list(re.finditer(r"\b" + re.escape(call) + r"\s*\(", body))
    return ms[-1].start() if ms else -1


def ordered(body: str, calls: list[str]) -> tuple[bool, str]:
    pos = [first(body, c) for c in calls]
    if any(p < 0 for p in pos):
        return False, "missing call(s): " + ", ".join(c for c, p in zip(calls, pos) if p < 0)
    if pos != sorted(pos) or len(set(pos)) != len(pos):
        return False, "wrong order: " + ", ".join(f"{c}@{p}" for c, p in zip(calls, pos))
    return True, "ok"


# ---- A. gb_g3home_restore_up ------------------------------------------------------------
def restore_order_facts(body: str) -> tuple[bool, str]:
    ok, d = ordered(body, ["xg_cell_is_copy", "app_can_edit", "app_gen3_pc_live", "xr_open",
                           "app_xfer_pending", "app_bank_defer_full", "gbsc_merge_up_sel",
                           "app_xfer_merge_screen"])
    if not ok:
        return False, d
    commit = last(body, "gbsc_merge_up_sel")
    setk = first(body, "app_xfer_pending_set")
    if setk < 0 or setk < commit or setk < first(body, "app_xfer_merge_screen"):
        return False, "app_xfer_pending_set must come after the merge screen AND the commit"
    if last(body, "app_xfer_pending_set") != setk:
        return False, "more than one app_xfer_pending_set"
    return True, "ok"


def restore_writes_nothing_facts(body: str) -> tuple[bool, str]:
    bad = [c for c in ("sf_write_verified", "f_unlink", "gbsc_remove", "gbsc_add", "gbsc_set_state",
                       "pdna_bank_next_serial", "app_bank_defer_delete") if first(body, c) >= 0]
    return (not bad), ("ok" if not bad else "writes to the card / bank: " + ", ".join(bad))


def restore_kind_facts(body: str) -> tuple[bool, str]:
    if "XR_KIND_G3_HOME" not in body:
        return False, "does not search XR_KIND_G3_HOME"
    if "XR_KIND_NATIVE_HOME" in body:
        return False, "must not touch NATIVE_HOME entries (the bridge arm's job)"
    if not re.search(r"state\s*!=\s*XR_STATE_NONE", body):
        return False, "no `state != XR_STATE_NONE` gate (only a plain entry restores)"
    return True, "ok"


# ---- B. the wrappers --------------------------------------------------------------------
def wrapper_facts(body: str) -> tuple[bool, str]:
    ok, d = ordered(body, ["gb_g3home_restore_up", "gb_bank_down_gen3"])
    if not ok:
        return False, d
    if not re.search(r"rc\s*==\s*1\s*\)\s*return\s+BANK_DOWN_CONVERTED", body):
        return False, "rc == 1 must return BANK_DOWN_CONVERTED"
    if not re.search(r"rc\s*!=\s*0\s*\)\s*return\s+BANK_DOWN_REFUSED", body):
        return False, "rc != 0 (declined/failed) must return BANK_DOWN_REFUSED, never fall into a conversion"
    return True, "ok"


# ---- C. app_xfer_promote's G3_HOME consume ---------------------------------------------
def promote_consume_facts(body: str) -> tuple[bool, str]:
    g3 = body.find("XR_KIND_G3_HOME")
    nat = body.find("XR_KIND_NATIVE_HOME")
    if g3 < 0 or nat < 0 or g3 > nat:
        return False, "the G3_HOME consume branch must exist and precede the NATIVE_HOME check"
    seg = body[g3:nat]
    ok, d = ordered(seg, ["gbsc_remove", "sf_write_verified", "app_xfer_pending_drop"])
    if not ok:
        return False, "G3_HOME branch: " + d
    if first(seg, "f_unlink") < 0:
        return False, "G3_HOME branch: the last entry must unlink the file"
    if "XR_STATE_NONE" not in seg:
        return False, "G3_HOME branch: must require a plain (NONE) entry"
    return True, "ok"


def save_now_order_facts(body: str) -> tuple[bool, str]:
    m = re.search(r"if\s*\(\s*app_commit_pc\s*\(\s*\)\s*\)\s*\{", body)
    p = first(body, "app_xfer_promote")
    if not m or p < 0:
        return False, "app_xfer_save_now: `if (app_commit_pc()) {` or app_xfer_promote( missing"
    if p < m.end():
        return False, "app_xfer_promote( runs before the verified app_commit_pc() -- the G3_HOME unlink could precede the save"
    return True, "ok"


def undo_kind_facts(body: str) -> tuple[bool, str]:
    k = re.search(r"e\.kind\s*!=\s*XR_KIND_NATIVE_HOME", body)
    r = first(body, "gbsc_remove")
    if not k or r < 0 or k.start() > r:
        return False, "app_xfer_pending_undo: the NATIVE_HOME-only filter must precede gbsc_remove( (a declined save must never remove a G3_HOME entry)"
    return True, "ok"


# ---- D. gb_bridge_restore_up ------------------------------------------------------------
def bridge_order_facts(body: str) -> tuple[bool, str]:
    ok, d = ordered(body, ["app_can_edit", "xg_cell_is_copy", "xr_open", "xr_resolve_home",
                           "xr_merge_down_gb_sel", "app_xfer_merge_screen", "pdna_bank_next_serial",
                           "bank_restore_from_entry_gb", "gb_accept_down_hook", "gb_bridge_mark_restored"])
    if not ok:
        return False, d
    landed = re.search(r"if\s*\(\s*!landed\s*\)\s*return\s+-2", body)
    if not landed or landed.start() > first(body, "gb_bridge_mark_restored"):
        return False, "the `if (!landed) return -2` check must precede gb_bridge_mark_restored"
    if landed.start() < first(body, "gb_accept_down_hook"):
        return False, "the landed check sits above the landing"
    return True, "ok"


def bridge_state_facts(body: str) -> tuple[bool, str]:
    m = re.search(r"state\s*==\s*XR_STATE_RESTORED\s*\)\s*\{[^}]*return\s+0\s*;", body, re.DOTALL)
    if not m:
        return False, "an already-RESTORED entry must `return 0` (convert normally: never restore twice, never refuse)"
    if m.start() > first(body, "app_xfer_merge_screen"):
        return False, "the RESTORED gate must precede the merge screen"
    p = re.search(r"state\s*==\s*XR_STATE_PENDING", body)
    if not p or p.start() > first(body, "app_xfer_merge_screen"):
        return False, "the PENDING (SAVE FIRST) wall must precede the merge screen"
    if not re.search(r"home\.gen\s*!=\s*g_ed->s\.gen", body):
        return False, "no home-generation gate (only an original of THIS save's generation comes home)"
    if "XR_DIR_ABROAD_GB" not in body:
        return False, "no ABROAD_GB direction gate"
    if "XR_KIND_G3_HOME" in body:
        return False, "must not touch G3_HOME entries (the Gen-3 target's job)"
    return True, "ok"


def bridge_wrapper_facts(body: str) -> tuple[bool, str]:
    ok, d = ordered(body, ["gb_bridge_restore_up", "gb_bank_down_bridge"])
    if not ok:
        return False, d
    if not re.search(r"rc\s*==\s*1\s*\)\s*return\s+BANK_DOWN_LANDED", body):
        return False, "rc == 1 must return BANK_DOWN_LANDED"
    if not re.search(r"rc\s*!=\s*0\s*\)\s*return\s+BANK_DOWN_REFUSED", body):
        return False, "rc != 0 must return BANK_DOWN_REFUSED"
    return True, "ok"


# ---- F. the lift is a pass-through ------------------------------------------------------
LEDGER_CALLS = ("gb_has_sidecar", "gb_lift_restore", "xr_open", "xr_resolve_home", "gbsc_find",
                "gbsc_get", "gbsc_merge_up_sel", "xr_merge_down_gb_sel", "app_xfer_merge_screen",
                "bank_restore_from_entry_gb")


def passthrough_facts(body: str) -> tuple[bool, str]:
    bad = [c for c in LEDGER_CALLS if first(body, c) >= 0]
    return (not bad), ("ok" if not bad else "reads the ledger / restores: " + ", ".join(bad))


def retired_facts(gen12: str) -> tuple[bool, str]:
    gone = [n for n in ("gb_lift_restore", "gb_lift_restore_g3home", "gb_release_g3home",
                        "gb_release_restored_verify")
            if re.search(r"^[^\n;{}]*\b" + n + r"\s*\([^;{]*\)\s*\{", strip_comments(gen12), re.MULTILINE)]
    return (not gone), ("ok" if not gone else "retired function(s) are back: " + ", ".join(gone))


# ---- G. gb_bridge_mark_restored ---------------------------------------------------------
def mark_facts(body: str) -> tuple[bool, str]:
    ok, d = ordered(body, ["xr_open", "xr_resolve_home", "gbsc_set_state", "sf_write_verified",
                           "app_xv_cache_invalidate"])
    if not ok:
        return False, d
    if "XR_STATE_RESTORED" not in body:
        return False, "does not set XR_STATE_RESTORED"
    return True, "ok"


def real_texts():
    gen12, conv, main = (p.read_text() for p in (GEN12, CONVERT, MAIN))
    return (function_body(gen12, "gb_g3home_restore_up"),
            function_body(conv, "bank_down_convert_gen3"),
            function_body(conv, "bank_down_convert_gen3_party"),
            function_body(main, "app_xfer_promote"))


def real_texts2():
    gen12, conv = GEN12.read_text(), CONVERT.read_text()
    return (function_body(gen12, "gb_bridge_restore_up"), function_body(conv, "bank_down_convert_gb"),
            function_body(gen12, "gb_lift_pack"), function_body(gen12, "gb_release_up_hook"),
            function_body(gen12, "gb_bridge_mark_restored"), gen12)


def run_real() -> None:
    r, w, wp, pr = real_texts()
    check(bool(r), "gb_g3home_restore_up not found")
    check(bool(w) and bool(wp) and bool(pr), "wrapper/promote bodies not found")
    for label, fn, body in (("A1 order", restore_order_facts, r),
                            ("A2 writes-nothing", restore_writes_nothing_facts, r),
                            ("A3 kind/state", restore_kind_facts, r),
                            ("B1 wrapper", wrapper_facts, w),
                            ("B2 party wrapper", wrapper_facts, wp),
                            ("C promote", promote_consume_facts, pr),
                            ("C2 save-now order", save_now_order_facts, function_body(MAIN.read_text(), "app_xfer_save_now")),
                            ("C3 undo kind", undo_kind_facts, function_body(MAIN.read_text(), "app_xfer_pending_undo"))):
        ok, d = fn(body)
        check(ok, f"{label}: {d}")
    br, bw, lp, rel, mk, gen12 = real_texts2()
    for label, fn, body in (("D1 bridge order", bridge_order_facts, br),
                            ("D2 bridge state/gates", bridge_state_facts, br),
                            ("E bridge wrapper", bridge_wrapper_facts, bw),
                            ("F1 gb_lift_pack pass-through", passthrough_facts, lp),
                            ("F2 gb_release_up_hook pass-through", passthrough_facts, rel),
                            ("F3 retired functions", retired_facts, gen12),
                            ("G mark", mark_facts, mk)):
        check(bool(body), f"{label}: body not found")
        ok, d = fn(body)
        check(ok, f"{label}: {d}")


def mutate(body: str, old: str, new: str) -> str:
    assert old in body, f"mutation anchor not found: {old!r}"
    return body.replace(old, new, 1)


def self_test() -> None:
    """Each mutant must be caught by the SAME fact function the real check uses."""
    r, w, wp, pr = real_texts()
    muts = [
        # A: order / writes / kind
        ("MUT A1a: pending_set hoisted above the merge screen", restore_order_facts,
         mutate(r, "app_xfer_merge_screen(", "app_xfer_pending_set(key, 0); app_xfer_merge_screen(")),
        ("MUT A1b: SAVE NOW? wall (app_xfer_pending) removed", restore_order_facts,
         r.replace("app_xfer_pending()", "0")),
        ("MUT A1c: copy-cell gate removed", restore_order_facts, r.replace("xg_cell_is_copy(", "0 && (")),
        ("MUT A1d: write gate after the ledger read", restore_order_facts,
         mutate(r, "app_can_edit()", "1").replace("xr_open(", "app_can_edit(); xr_open(", 1)),
        ("MUT A2: the restore consumes the entry itself (pre-flush)", restore_writes_nothing_facts,
         mutate(r, "app_xfer_pending_set(", "gbsc_remove(buf, &len, found); app_xfer_pending_set(")),
        ("MUT A3a: the state gate dropped", restore_kind_facts,
         re.sub(r"e\.state\s*!=\s*XR_STATE_NONE", "0", r)),
        ("MUT A3b: searches NATIVE_HOME too", restore_kind_facts, r + " XR_KIND_NATIVE_HOME"),
        # B
        ("MUT B1a: wrapper converts BEFORE the restore", wrapper_facts,
         mutate(w, "gb_g3home_restore_up(", "gb_bank_down_gen3(0,0,0,0,0,0); gb_g3home_restore_up(")),
        ("MUT B1b: declined restore falls into a conversion", wrapper_facts,
         re.sub(r"if\s*\(rc\s*!=\s*0\)\s*return\s+BANK_DOWN_REFUSED;", "", w)),
        ("MUT B2: party wrapper never asks", wrapper_facts, wp.replace("gb_g3home_restore_up(", "0 && (")),
        # C
        ("MUT C1: consume writes before removing", promote_consume_facts,
         mutate(pr, "gbsc_remove(", "sf_write_verified(path, s_promote_buf, len); gbsc_remove(")),
        ("MUT C2: no G3_HOME branch", promote_consume_facts, pr.replace("XR_KIND_G3_HOME", "XR_KIND_XXX")),
        ("MUT C3: last entry not unlinked", promote_consume_facts, pr.replace("f_unlink", "f_xunlink")),
    ]
    br, bw, lp, rel, mk, gen12 = real_texts2()
    muts += [
        ("MUT D1a: the RESTORED mark before the landing", bridge_order_facts,
         mutate(br, "gb_accept_down_hook(", "gb_bridge_mark_restored(&mon); gb_accept_down_hook(")),
        ("MUT D1a2: the landed check removed (mark runs even when nothing landed)", bridge_order_facts,
         re.sub(r"if\s*\(\s*!landed\s*\)\s*return\s+-2\s*;", "", br)),
        ("MUT D1b: landing before the merge screen", bridge_order_facts,
         mutate(br, "app_xfer_merge_screen(", "gb_accept_down_hook(dst_box, cell80); app_xfer_merge_screen(")),
        ("MUT D1c: copy gate dropped", bridge_order_facts, br.replace("xg_cell_is_copy(", "0 && (")),
        ("MUT D2a: an already-RESTORED entry refuses instead of converting", bridge_state_facts,
         re.sub(r"(state\s*==\s*XR_STATE_RESTORED\s*\)\s*\{[^}]*return\s+)0(\s*;)", r"\g<1>-2\2", br, flags=re.DOTALL)),
        ("MUT D2b: home-generation gate dropped", bridge_state_facts,
         re.sub(r"home\.gen\s*!=\s*g_ed->s\.gen", "0", br)),
        ("MUT D2c: PENDING wall dropped", bridge_state_facts, br.replace("XR_STATE_PENDING", "XR_STATE_XXX")),
        ("MUT E1: wrapper converts first", bridge_wrapper_facts,
         mutate(bw, "gb_bridge_restore_up(", "gb_bank_down_bridge(0,0); gb_bridge_restore_up(")),
        ("MUT E2: wrapper falls into a conversion after a declined restore", bridge_wrapper_facts,
         re.sub(r"if\s*\(rc\s*!=\s*0\)\s*return\s+BANK_DOWN_REFUSED;", "", bw)),
        ("MUT F1a: the lift asks the ledger again", passthrough_facts, lp + " gb_has_sidecar(0,0);"),
        ("MUT F1b: the lift restores again", passthrough_facts, lp + " gb_lift_restore(0,0);"),
        ("MUT F2: the release re-reads the ledger", passthrough_facts, rel + " xr_open(0,0,0,0,0);"),
        ("MUT F3: gb_release_g3home is back", retired_facts,
         gen12 + "\nstatic bool gb_release_g3home(int box, int slot, const GbEditMon* have) {\n}\n"),
        ("MUT G1: marked before the verified write order", mark_facts,
         mutate(mk, "gbsc_set_state(", "sf_write_verified(0,0,0); gbsc_set_state(")),
    ]
    for label, fn, body in muts:
        ok, d = fn(body)
        check(not ok, f"{label} should have been caught but was not")
        print(f"  {label}: {'CAUGHT' if not ok else 'MISSED'} ({d})")


def main() -> int:
    run_real()
    self_test()
    print(f"host_y9_target_restore_sites_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
