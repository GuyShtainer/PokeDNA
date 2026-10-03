#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zx_sites_test.py -- structural pins for lane zx (BACKLOG #374/#375, later #370/#372/#376).
Pure text checks against the shipped source (no build). Every fact function is shared by the real
check and by an in-memory MUTATION that must turn it red (self-test at the bottom).

  #372a-c    the MOVE-mode grab refusal goes through grab_refused() (beep + GB why-toast + log line); a card
             write failure at the first meta write is XG_LIFT_CARD -> its own "could not write to the card"
             dialog, not the packing one; BANK RECORD CLASH carries a second (next-step) line.
  #374/#375  app_xfer_save_now (source/pdna_main.c): after a CONFIRMED app_commit_pc(), a failed
             promotion shows PDNA_XFER_LEDGER_* ("saved, ledger not updated"), never PDNA_XFER_NOTSAVED_*
             (that panel stays on the commit-FAILED arm), keeps ok = promoted (#175 D1: the caller still
             refuses a second transfer), and the log line names the real outcome.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "source" / "pdna_main.c"
BOX = ROOT / "source" / "pdna_box.c"
GEN12 = ROOT / "source" / "pdna_gen12.c"
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
            i = n if j < 0 else j
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


def save_now_facts(body: str) -> tuple[bool, str]:
    m = re.search(r"if\s*\(\s*app_commit_pc\s*\(\s*\)\s*\)\s*\{", body)
    if not m:
        return False, "app_xfer_save_now: `if (app_commit_pc()) {` missing"
    depth, j = 1, m.end()
    while j < len(body) and depth > 0:
        depth += (body[j] == "{") - (body[j] == "}")
        j += 1
    ok_arm, fail_arm = body[m.end():j], body[j:]
    if not re.search(r"if\s*\(\s*!promoted\s*\)\s*\{[^}]*PDNA_XFER_LEDGER_TITLE", ok_arm):
        return False, "the !promoted branch does not show PDNA_XFER_LEDGER_TITLE (#374)"
    if "PDNA_XFER_NOTSAVED" in ok_arm:
        return False, "the confirmed-save arm still shows PDNA_XFER_NOTSAVED_* (#374: the save WAS confirmed)"
    if "PDNA_XFER_NOTSAVED_TITLE" not in fail_arm:
        return False, "the commit-FAILED arm lost PDNA_XFER_NOTSAVED_TITLE"
    if not re.search(r"ok\s*=\s*promoted\s*;", ok_arm):
        return False, "`ok = promoted;` missing (#175 D1: a failed promotion must refuse a second transfer)"
    lg = re.search(r'log_line\(\s*promoted\s*\?\s*(?:\(\s*had_pending\s*\?\s*)?"xfer: save-now: committed, entry promoted"(?:\s*:\s*"xfer: save-now: committed, nothing pending"\s*\))?\s*:\s*"xfer: save-now: committed, promotion FAILED', ok_arm)
    if not lg:
        return False, "the save-now log line does not branch on `promoted` (#375)"
    return True, "ok"


def grab_facts(box_text: str, gen12_text: str) -> tuple[bool, str]:
    t = strip_comments(box_text)
    if not re.search(r"s_cur_mode\s*==\s*CM_MOVE\)\s*\{\s*if\s*\(\s*!src_can_lift\(src,\s*box,\s*cur\)\)\s*grab_refused\(src,\s*box,\s*cur,\s*toast\)", t):
        return False, "the CM_MOVE A-grab refusal does not call grab_refused(src, box, cur, toast) (#372a)"
    gr = function_body(box_text, "grab_refused")
    if not gr or "gb_lift_why_note(" not in gr or "log_line(" not in gr or "toast" not in gr or "snd_deny()" not in gr:
        return False, "grab_refused() must beep, query gb_lift_why_note, queue the toast and log_line (#372a)"
    dh = function_body(box_text, "drop_held_up")
    if not re.search(r"XG_LIFT_CARD\)\s*\{[^}]*PDNA_XFER_LIFT_CARD_L1", dh):
        return False, "drop_held_up: XG_LIFT_CARD does not show PDNA_XFER_LIFT_CARD_L1 (#372b)"
    if "PDNA_BANK_COLL_L2" not in dh:
        return False, "drop_held_up: BANK RECORD CLASH lost its next-step line PDNA_BANK_COLL_L2 (#372c)"
    lp = function_body(gen12_text, "gb_lift_pack")
    if not re.search(r"pdna_bank_next_serial\(\);\s*if\s*\(!serial\)\s*\{[^}]*return XG_LIFT_CARD;", lp):
        return False, "gb_lift_pack: a failed pdna_bank_next_serial() must return XG_LIFT_CARD (#372b)"
    return True, "ok"


def mutate(body: str, old: str, new: str) -> str:
    assert old in body, f"mutation anchor not found: {old!r}"
    return body.replace(old, new, 1)


def main() -> int:
    body = function_body(MAIN.read_text(), "app_xfer_save_now")
    check(bool(body), "app_xfer_save_now body not found")
    ok, d = save_now_facts(body)
    check(ok, d)
    # mutations: each must turn the facts red
    for label, old, new in (
        ("374: !promoted shows NOTSAVED again", "PDNA_XFER_LEDGER_TITLE, UI_WARN, PDNA_XFER_LEDGER_L1, PDNA_XFER_LEDGER_L2",
         "PDNA_XFER_NOTSAVED_TITLE, UI_WARN, PDNA_XFER_NOTSAVED_L1, PDNA_XFER_NOTSAVED_L2"),
        ("375: log unconditional again", 'log_line(promoted ? (had_pending ? "xfer: save-now: committed, entry promoted" : "xfer: save-now: committed, nothing pending")', 'log_line("xfer: save-now: committed, entry promoted"'),
        ("175D1: ok no longer follows promoted", "ok = promoted;", "ok = true;"),
    ):
        if old not in body:
            check(False, f"mutation anchor missing ({label})")
            continue
        mo = mutate(body, old, new)
        if label.startswith("375"):
            mo = re.sub(r'\)?\s*\n\s*: "xfer: save-now: committed, promotion FAILED \(ledger entry stays pending\)"\);', ');', mo)
        ok, _ = save_now_facts(mo)
        check(not ok, f"MUT {label} was NOT caught")
    bt, gt = BOX.read_text(), GEN12.read_text()
    ok, d = grab_facts(bt, gt)
    check(ok, d)
    for label, which, old, new in (
        ("372a: grab refusal back to a bare beep", "box", "grab_refused(src, box, cur, toast);", "snd_deny();"),
        ("372b: card failure back to FAILED", "gen12", "return XG_LIFT_CARD;", "return XG_LIFT_FAILED;"),
        ("372c: clash line dropped", "box", "PDNA_BANK_COLL_L1, PDNA_BANK_COLL_L2", "PDNA_BANK_COLL_L1, NULL"),
    ):
        src = bt if which == "box" else gt
        if old not in src:
            check(False, f"mutation anchor missing ({label})")
            continue
        mb, mg = (mutate(bt, old, new), gt) if which == "box" else (bt, mutate(gt, old, new))
        ok, _ = grab_facts(mb, mg)
        check(not ok, f"MUT {label} was NOT caught")
    print(f"host_zx_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
