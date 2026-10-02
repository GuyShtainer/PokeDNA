#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zx_sites_test.py -- structural pins for lane zx (BACKLOG #374/#375, later #370/#372/#376).
Pure text checks against the shipped source (no build). Every fact function is shared by the real
check and by an in-memory MUTATION that must turn it red (self-test at the bottom).

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
    lg = re.search(r'log_line\(\s*promoted\s*\?\s*"xfer: save-now: committed, entry promoted"\s*:\s*"xfer: save-now: committed, promotion FAILED', ok_arm)
    if not lg:
        return False, "the save-now log line does not branch on `promoted` (#375)"
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
        ("375: log unconditional again", 'log_line(promoted ? "xfer: save-now: committed, entry promoted"', 'log_line("xfer: save-now: committed, entry promoted"'),
        ("175D1: ok no longer follows promoted", "ok = promoted;", "ok = true;"),
    ):
        if old not in body:
            check(False, f"mutation anchor missing ({label})")
            continue
        mo = mutate(body, old, new)
        if label.startswith("375"):
            mo = mo.replace('\n                      : "xfer: save-now: committed, promotion FAILED (ledger entry stays pending)");', ');')
        ok, _ = save_now_facts(mo)
        check(not ok, f"MUT {label} was NOT caught")
    print(f"host_zx_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
