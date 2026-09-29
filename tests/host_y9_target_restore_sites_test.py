#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_y9_target_restore_sites_test.py -- structural pins for BACKLOG #280 (Guy's #270 ruling,
the GB->Bank half): the lift is a pass-through and the original comes home at the TARGET drop.
Pure text checks against the shipped source (no build), the same posture as
host_escape_gate_sites_test.py's Y7 pins. Every fact function is shared by the real check and by
an in-memory MUTATION that must turn it red (the self-test at the bottom).

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


def real_texts():
    gen12, conv, main = (p.read_text() for p in (GEN12, CONVERT, MAIN))
    return (function_body(gen12, "gb_g3home_restore_up"),
            function_body(conv, "bank_down_convert_gen3"),
            function_body(conv, "bank_down_convert_gen3_party"),
            function_body(main, "app_xfer_promote"))


def run_real() -> None:
    r, w, wp, pr = real_texts()
    check(bool(r), "gb_g3home_restore_up not found")
    check(bool(w) and bool(wp) and bool(pr), "wrapper/promote bodies not found")
    for label, fn, body in (("A1 order", restore_order_facts, r),
                            ("A2 writes-nothing", restore_writes_nothing_facts, r),
                            ("A3 kind/state", restore_kind_facts, r),
                            ("B1 wrapper", wrapper_facts, w),
                            ("B2 party wrapper", wrapper_facts, wp),
                            ("C promote", promote_consume_facts, pr)):
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
