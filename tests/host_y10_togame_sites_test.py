#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_y10_togame_sites_test.py -- structural pins for BACKLOG #271's remainder (lane y10-togame):
TO GAME on a NATIVE Bank cell in a Gen-3 session runs THE DROP'S OWN ARM and lands what a drop lands.
Pure text checks against the shipped source (no build), the same posture as
host_y9_target_restore_sites_test.py. Every fact function is shared by the real check and by an
in-memory MUTATION that must turn it red (the self-test at the bottom).

  A. the menu row: native branch of app_mon_menu offers TO GAME between EXPORT and RELEASE, gated by
     xg_togame_row(is_bank, ...); the whitelist admits A_TOGAME; the action on a native cell only sets
     the one-shot request (never app_inject_to_game, which would memcpy raw GBC1 bytes into the PC).
  B. the consumers (pdna_box.c): both menu call sites consume the request through togame_native_run, whose
     body refuses off the Bank grid before calling app_bank_togame_native.
  C. app_bank_togame_native: write/PC-live gates and the defer-queue room check BEFORE the dispatch; the
     dispatch BEFORE any placement; it calls bank_down_dispatch (never an arm's internals directly); only
     BANK_DOWN_CONVERTED places; it never writes the save itself.
  D. MENU-vs-DROP PARITY (the same fixture, both bodies parsed): bank_down_dispatch is called with the same
     six-argument shape by drop_held's BANK -> PC branch and by app_bank_togame_native; the sixth (out)
     buffer is the record then placed, never the native cell; the placement sequence
     [dex note, 80-byte copy, dirty mark, deferred Bank delete of the ORIGINAL cell] is the same in both --
     the drop's `src->note_add` / `src->mark_dirty` are proven to be app_register_dex_deferred /
     app_mark_pc_dirty by the PC BoxSource's own table (pc_box_source, pcsrc_note_add).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "source" / "pdna_main.c"
BOX = ROOT / "source" / "pdna_box.c"
CONV = ROOT / "source" / "bank_down_convert.c"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip_comments(text: str) -> str:
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


def ordered(body: str, calls: list[str]) -> tuple[bool, str]:
    pos = [first(body, c) for c in calls]
    if any(p < 0 for p in pos):
        return False, "missing call(s): " + ", ".join(c for c, p in zip(calls, pos) if p < 0)
    if pos != sorted(pos) or len(set(pos)) != len(pos):
        return False, "wrong order: " + ", ".join(f"{c}@{p}" for c, p in zip(calls, pos))
    return True, "ok"


def brace_block(body: str, start: int) -> str:
    """The `{...}` block whose opening brace is the first '{' at/after `start`."""
    i = body.find("{", start)
    if i < 0:
        return ""
    depth, j = 1, i + 1
    while j < len(body) and depth > 0:
        depth += (body[j] == "{") - (body[j] == "}")
        j += 1
    return body[i:j]


def call_args(body: str, call: str) -> list[str]:
    """Top-level comma-split arguments of the first `call(...)`."""
    m = re.search(r"\b" + re.escape(call) + r"\s*\(", body)
    if not m:
        return []
    depth, j, cur, args = 1, m.end(), "", []
    while j < len(body) and depth > 0:
        c = body[j]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
            if depth == 0:
                break
        if c == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
        j += 1
    args.append(cur.strip())
    return args


# ---- A. the menu row -----------------------------------------------------------------------
def menu_row_facts(menu: str) -> tuple[bool, str]:
    nat = menu.find("PDNA_LBL_EXPORT_PK; act[n++]=A_EXPORT;")
    end = menu.find("} else if (occupied) {")
    if nat < 0 or end < 0 or end < nat:
        return False, "the native branch (EXPORT ... `} else if (occupied) {`) was not found"
    seg = menu[nat:end]
    t = seg.find("A_TOGAME")
    r = seg.find("A_RELEASE")
    if t < 0:
        return False, "the native branch has no TO GAME row (A_TOGAME)"
    if r < 0 or t > r:
        return False, "TO GAME must sit before RELEASE in the native branch"
    if not re.search(r"if\s*\([^;{]*xg_togame_row\(is_bank[^;{]*\)\s*\{\s*lab\[n\]=PDNA_LBL_TO_GAME;\s*act\[n\+\+\]=A_TOGAME;", seg):
        return False, "the native TO GAME row is not gated by xg_togame_row(is_bank, ...) (Omega-write + live Gen-3 PC gates)"
    return True, "ok"


def whitelist_facts(menu: str) -> tuple[bool, str]:
    m = re.search(r"if\s*\(\s*native\s*&&[^\n]*act\[sel\]\s*!=\s*A_CANCEL\s*\)\s*\{\s*snd_deny\(\)\s*;\s*return\s+false\s*;", menu)
    if not m:
        return False, "the native whitelist line (snd_deny + return false) was not found"
    if "act[sel] != A_TOGAME" not in m.group(0):
        return False, "the native whitelist does not admit A_TOGAME"
    return True, "ok"


def action_facts(menu: str) -> tuple[bool, str]:
    m = re.search(r"case\s+A_TOGAME:\s*if\s*\(\s*native\s*\)\s*\{\s*g_togame_req\s*=\s*true\s*;\s*return\s+false\s*;\s*\}\s*return\s+app_inject_to_game\(", menu)
    if not m:
        return False, "case A_TOGAME must be `if (native) { g_togame_req = true; return false; }` BEFORE app_inject_to_game(rec) (a native cell's raw bytes never reach the PC)"
    return True, "ok"


# ---- B. the consumers ----------------------------------------------------------------------
def consumer_facts(box: str) -> tuple[bool, str]:
    b = strip_comments(box)
    sites = [m.start() for m in re.finditer(r"app_take_togame_request\s*\(\s*\)", b)]
    if len(sites) != 2:
        return False, f"expected the request consumed at exactly the 2 menu call sites, found {len(sites)}"
    for s in sites:
        if "togame_native_run(" not in b[s:s + 200]:
            return False, "a consumer site does not call togame_native_run( right after taking the request"
    return True, "ok"


def helper_facts(body: str) -> tuple[bool, str]:
    if not body:
        return False, "togame_native_run not found"
    g = re.search(r"if\s*\(\s*!src->is_bank", body)
    c = first(body, "app_bank_togame_native")
    if not g or c < 0 or g.start() > c:
        return False, "togame_native_run must refuse off the Bank grid (`!src->is_bank`) before calling app_bank_togame_native"
    if first(body, "boxoam_suspend") < 0 or first(body, "boxoam_resume") < 0 or first(body, "boxoam_suspend") > c:
        return False, "the arm's screens/card I/O must run inside a boxoam_suspend/resume bracket"
    return True, "ok"


# ---- C. app_bank_togame_native -------------------------------------------------------------
def scan_facts(body: str) -> tuple[bool, str]:
    """H1: a destination cell is free only when ALL 80 bytes are zero (never just the personality)."""
    if not body:
        return False, "app_bank_togame_native not found"
    if re.search(r"cs\[0\]\s*\|\s*cs\[1\]", body):
        return False, "the scan tests only the 4 personality bytes: a PID-0 mon / Bad Egg would be overwritten"
    if not re.search(r"k\s*<\s*80\s*&&\s*zero\s*;", body):
        return False, "the free-cell scan must fold over ALL 80 bytes with early exit (`k < 80 && zero;` -- a bare `k < 80` lets byte 79 alone decide)"
    if not re.search(r"zero\s*=\s*\(\s*cs\[k\]\s*==\s*0\s*\)", body):
        return False, "the free-cell scan must fold zero = (cs[k] == 0)"
    if not re.search(r"if\s*\(\s*zero\s*\)", body):
        return False, "the free cell must be taken only under `if (zero)`"
    return True, "ok"


def togame_order_facts(body: str) -> tuple[bool, str]:
    if not body:
        return False, "app_bank_togame_native not found"
    ok, d = ordered(body, ["bc_is_native", "xg_inject_refuse", "app_can_edit", "app_bank_defer_full",
                           "bank_down_dispatch", "app_register_dex_deferred",
                           "app_mark_pc_dirty", "app_bank_defer_delete"])
    if not ok:
        return False, d
    # the FIRST memcpy is the local copy of the cell (before the dispatch); the placement memcpy is the LAST
    ms = [m.start() for m in re.finditer(r"\bmemcpy\s*\(", body)]
    disp = first(body, "bank_down_dispatch")
    if len(ms) < 2 or ms[-1] < disp or ms[0] > disp:
        return False, "expected a local copy of the cell BEFORE the dispatch and the placement memcpy AFTER it"
    if not re.search(r"bd\s*!=\s*BANK_DOWN_CONVERTED[^;]*return\s+false", body):
        return False, "only BANK_DOWN_CONVERTED may place: `bd != BANK_DOWN_CONVERTED ... return false` missing"
    if "bc_is_native(conv)" not in body:
        return False, "the finished record is not checked non-native before it is placed (a native cell must never land in the PC)"
    return True, "ok"


ARM_INTERNALS = ("gb_g3home_restore_up", "gb_bank_down_gen3", "bank_down_convert_gen3", "gb_bridge_restore_up",
                 "bank_down_convert_gb", "xfer_down_write", "app_inject_to_game", "app_inject_to_game_deferred")
SAVE_WRITES = ("app_commit_pc", "app_commit_all", "app_commit_sb1", "sf_write_verified", "box_save",
               "pdna_bank_flush_deletions")


def togame_isolation_facts(body: str) -> tuple[bool, str]:
    bad = [c for c in ARM_INTERNALS + SAVE_WRITES if first(body, c) >= 0]
    return (not bad), ("ok" if not bad else "calls an arm internal / writes the save itself: " + ", ".join(bad))


# ---- D. parity -----------------------------------------------------------------------------
def drop_pc_branch(drop_body: str) -> str:
    m = re.search(r"if\s*\(\s*src->scope\s*==\s*BOXSCOPE_PC\s*&&\s*s_orig_scope\s*==\s*BOXSCOPE_BANK\s*&&\s*s_orig_slot\s*>=\s*0\s*\)\s*\{", drop_body)
    return brace_block(drop_body, m.start()) if m else ""


def drop_dispatch_block(drop_body: str) -> str:
    m = re.search(r"if\s*\(\s*s_orig_scope\s*==\s*BOXSCOPE_BANK[^{]*\{", drop_body)
    return brace_block(drop_body, m.start()) if m else ""


def sequence(body: str, names: dict) -> list[str]:
    """Positions of the canonical placement steps, in text order, using the per-side spelling."""
    pos = []
    for canon, spell in names.items():
        p = first(body, spell)
        if p < 0:
            return []
        pos.append((p, canon))
    return [c for _, c in sorted(pos)]


def parity_facts(drop_body: str, menu_body: str, pc_src: str, pcsrc_note: str) -> tuple[bool, str]:
    dd = drop_dispatch_block(drop_body)
    pb = drop_pc_branch(drop_body)
    if not dd or not pb or not menu_body:
        return False, "a parity body was not found (drop dispatch block / BANK->PC branch / app_bank_togame_native)"
    da, ma = call_args(dd, "bank_down_dispatch"), call_args(menu_body, "bank_down_dispatch")
    if len(da) != 6 or len(ma) != 6:
        return False, f"bank_down_dispatch must be called with 6 arguments on both sides (drop {len(da)}, menu {len(ma)})"
    # the sixth argument is the out buffer: it is the record PLACED, on both sides
    d_out, m_out = da[5], ma[5]
    if not re.search(r"placing\s*=\s*converted\s*\?\s*" + re.escape(d_out) + r"\b", pb):
        return False, f"drop side: the placed record is not the dispatch's out buffer `{d_out}` when converted"
    if not re.search(r"memcpy\s*\(\s*[^;]*,\s*" + re.escape(m_out) + r"\s*,\s*80\s*\)", menu_body):
        return False, f"menu side: the placed record is not the dispatch's out buffer `{m_out}`"
    # the drop's dispatch-arm condition is the one the menu shares: the arm is chosen by xg_bank_down_arm, not by the caller
    if "bank_down_dispatch" in menu_body and "xg_bank_down_arm" in menu_body:
        return False, "the menu must not pick the arm itself (bank_down_dispatch owns xg_bank_down_arm)"
    # placement sequence (canonical: dex note, copy, dirty, deferred delete)
    dseq = sequence(pb, {"dex": "src->note_add", "copy": "memcpy", "dirty": "src->mark_dirty",
                         "defer": "app_bank_defer_delete"})
    mseq = sequence(menu_body[first(menu_body, "bank_down_dispatch"):],
                    {"dex": "app_register_dex_deferred", "copy": "memcpy", "dirty": "app_mark_pc_dirty",
                     "defer": "app_bank_defer_delete"})
    # drop: note_add precedes the memcpy; the menu's memcpy precedes the dex note -- normalise: same SET, same
    # RELATIVE order of {dirty, defer}, and both end with the deferred delete
    if not dseq or not mseq or set(dseq) != set(mseq):
        return False, f"placement steps differ: drop {dseq} vs menu {mseq}"
    if dseq[-1] != "defer" or mseq[-1] != "defer" or dseq.index("dirty") > dseq.index("defer") or mseq.index("dirty") > mseq.index("defer"):
        return False, "both sides must mark the PC dirty and end with the deferred Bank delete"
    # the deferred delete removes the ORIGINAL native cell, never the converted record
    dd_args = call_args(pb, "app_bank_defer_delete")
    md_args = call_args(menu_body, "app_bank_defer_delete")
    if len(dd_args) != 3 or len(md_args) != 3 or dd_args[2] != "s_held" or md_args[2] == m_out:
        return False, "the deferred delete must be handed the ORIGINAL cell (drop: s_held), not the converted record"
    # the drop's note_add / mark_dirty ARE the calls the menu makes (through the PC BoxSource's own table)
    if not re.search(r"s\.note_add\s*=\s*pcsrc_note_add", pc_src):
        return False, "pc_box_source no longer installs pcsrc_note_add as note_add"
    if not re.search(r"s\.mark_dirty\s*=\s*app_mark_pc_dirty", pc_src):
        return False, "pc_box_source no longer installs app_mark_pc_dirty as mark_dirty"
    if not re.search(r"app_register_dex_deferred\s*\(\s*rec\s*,\s*false\s*\)", pcsrc_note):
        return False, "pcsrc_note_add is no longer app_register_dex_deferred(rec, false) -- the menu's dex step drifted from the drop's"
    return True, "ok"


# ---- E. bank_down_dispatch is the ONE switch -------------------------------------------------
def dispatch_facts(box: str, conv: str) -> tuple[bool, str]:
    d = function_body(box, "bank_down_dispatch")
    if not d:
        return False, "bank_down_dispatch not found"
    if re.search(r"^[ \t]*static\b[^;{}]*?\bbank_down_dispatch\s*\(", strip_comments(box), re.MULTILINE | re.DOTALL):
        return False, "bank_down_dispatch is static again -- app_bank_togame_native (pdna_main.c) cannot share it"
    if "XG_DOWN_ARM_GEN3" not in d or "bank_down_convert_gen3(" not in d:
        return False, "the GEN3 arm no longer routes through bank_down_convert_gen3"
    return True, "ok"


def real_texts():
    main, box = MAIN.read_text(), BOX.read_text()
    menu = function_body(main, "app_mon_menu")
    return dict(
        menu=menu,
        togame=function_body(main, "app_bank_togame_native"),
        helper=function_body(box, "togame_native_run"),
        drop=function_body(box, "drop_held"),
        pc_src=function_body(main, "pc_box_source"),
        pcsrc_note=function_body(main, "pcsrc_note_add"),
        box=box, conv=CONV.read_text(),
    )


def run_real() -> None:
    t = real_texts()
    for k in ("menu", "togame", "helper", "drop", "pc_src", "pcsrc_note"):
        check(bool(t[k]), f"{k}: body not found")
    for label, fn, arg in (("A1 menu row", menu_row_facts, t["menu"]),
                           ("A2 whitelist", whitelist_facts, t["menu"]),
                           ("A3 action", action_facts, t["menu"]),
                           ("B1 consumers", consumer_facts, t["box"]),
                           ("B2 helper", helper_facts, t["helper"]),
                           ("C1 order", togame_order_facts, t["togame"]),
                           ("C3 all-80-zero scan", scan_facts, t["togame"]),
                           ("C2 isolation", togame_isolation_facts, t["togame"]),
                           ("E dispatch", lambda _b: dispatch_facts(t["box"], t["conv"]), None)):
        ok, d = fn(arg)
        check(ok, f"{label}: {d}")
    ok, d = parity_facts(t["drop"], t["togame"], t["pc_src"], t["pcsrc_note"])
    check(ok, f"D parity: {d}")


def mutate(body: str, old: str, new: str) -> str:
    assert old in body, f"mutation anchor not found: {old!r}"
    return body.replace(old, new, 1)


def self_test() -> None:
    t = real_texts()
    menu, tg, hp, dr, ps, pn = t["menu"], t["togame"], t["helper"], t["drop"], t["pc_src"], t["pcsrc_note"]
    box = t["box"]
    muts = [
        ("MUT A1a: the native TO GAME row removed", menu_row_facts,
         mutate(menu, "if (is_bank && xg_togame_row(is_bank, app_gen3_pc_live(), g_have_pc)) { lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; }", "")),
        ("MUT A1b: the row loses its Omega/live-PC gate", menu_row_facts,
         mutate(menu, "if (is_bank && xg_togame_row(is_bank, app_gen3_pc_live(), g_have_pc)) { lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; }",
                "if (is_bank) { lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; }")),
        ("MUT A2: the whitelist drops A_TOGAME", whitelist_facts, mutate(menu, "act[sel] != A_TOGAME && ", "")),
        ("MUT A3a: a native cell injects its raw bytes", action_facts,
         mutate(menu, "case A_TOGAME:  if (native) { g_togame_req = true; return false; }", "case A_TOGAME:  if (0) { g_togame_req = true; return false; }")),
        ("MUT A3b: the native action never sets the request", action_facts, mutate(menu, "g_togame_req = true; return false; }   ", "return false; }   ")),
        ("MUT B1: one consumer site removed", consumer_facts,
         box.replace("app_take_togame_request()", "app_take_dup_request()", 1)),
        ("MUT B2a: the helper runs off the Bank", helper_facts, mutate(hp, "!src->is_bank", "0")),
        ("MUT B2b: no OAM bracket", helper_facts, hp.replace("boxoam_suspend();", "", 1)),
        ("MUT C1a: dispatch before the write gate", togame_order_facts,
         tg.replace("if (!app_can_edit())", "if (0)", 1).replace("bank_down_dispatch(", "app_can_edit(); bank_down_dispatch(", 1)),
        ("MUT C1b: defer-queue room check removed", togame_order_facts, tg.replace("app_bank_defer_full()", "0")),
        ("MUT C1c: a refused arm still places", togame_order_facts,
         re.sub(r"bd\s*!=\s*BANK_DOWN_CONVERTED\s*\|\|\s*bc_is_native\(conv\)", "bc_is_native(conv)", tg)),
        ("MUT C1d: native record check removed", togame_order_facts, tg.replace("bc_is_native(conv)", "0")),
        ("MUT C2a: TO GAME calls the restore directly (a clone of the arm)", togame_isolation_facts,
         tg + " gb_g3home_restore_up(0,0);"),
        ("MUT C2b: TO GAME saves the PC itself", togame_isolation_facts, tg + " app_commit_pc();"),
        ("MUT C3a: any cell is free", scan_facts,
         mutate(tg, "if (zero) { db = b; ds = c; }", "if (1) { db = b; ds = c; }")),
        ("MUT C3b: the 4-byte personality test is back", scan_facts,
         mutate(tg, "if (zero) { db = b; ds = c; }", "if ((cs[0] | cs[1] | cs[2] | cs[3]) == 0) { db = b; ds = c; }")),
        ("MUT C3c: byte 79 alone decides (early exit dropped)", scan_facts,
         mutate(tg, "k < 80 && zero", "k < 80")),
        ("MUT D1a: the menu places the NATIVE cell", parity_facts,
         (dr, tg.replace("memcpy(dst, conv, 80)", "memcpy(dst, held, 80)"), ps, pn)),
        ("MUT D1b: the menu deletes the converted record instead of the original", parity_facts,
         (dr, tg.replace("app_bank_defer_delete(bank_box, bank_slot, held)", "app_bank_defer_delete(bank_box, bank_slot, conv)"), ps, pn)),
        ("MUT D1c: the menu never queues the Bank delete", parity_facts,
         (dr, tg.replace("app_bank_defer_delete(bank_box, bank_slot, held);", ";"), ps, pn)),
        ("MUT D1d: the menu forgets the dex note", parity_facts,
         (dr, tg.replace("app_register_dex_deferred(conv, false);", ";"), ps, pn)),
        ("MUT D1e: the menu passes a different out buffer", parity_facts,
         (dr, tg.replace("bank_down_dispatch(&pcs, db, ds, held, dst, conv)", "bank_down_dispatch(&pcs, db, ds, held, dst, held)"), ps, pn)),
        ("MUT D2a: the PC BoxSource note_add drifts", parity_facts,
         (dr, tg, ps.replace("pcsrc_note_add", "other_note_add"), pn)),
        ("MUT D2b: pcsrc_note_add drifts from the menu's dex call", parity_facts,
         (dr, tg, ps, pn.replace("app_register_dex_deferred", "app_register_dex_now"))),
        ("MUT D3: the drop stops deleting the original", parity_facts,
         (dr.replace("app_bank_defer_delete(s_orig_box, s_orig_slot, s_held)", "app_bank_defer_delete(s_orig_box, s_orig_slot, conv)"), tg, ps, pn)),
        ("MUT E: bank_down_dispatch is static again", dispatch_facts,
         (mutate(box, "BankDownResult __attribute__((noinline))\nbank_down_dispatch(", "static BankDownResult __attribute__((noinline))\nbank_down_dispatch("), t["conv"])),
    ]
    for label, fn, arg in muts:
        ok, d = fn(*arg) if isinstance(arg, tuple) else fn(arg)
        check(not ok, f"{label} should have been caught but was not")
        print(f"  {label}: {'CAUGHT' if not ok else 'MISSED'} ({d})")


def main() -> int:
    run_real()
    self_test()
    print(f"host_y10_togame_sites_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
