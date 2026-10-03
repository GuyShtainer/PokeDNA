#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zz_sites_test.py -- structural pins for lane zz (BACKLOG #383 #384 #385 #377 #382).
Pure text checks against the shipped source (no build). Every fact function is shared by the real
check and by in-memory MUTATIONS that must turn it red (self-test in main()).

  #383  app_xfer_save_now promotes ONLY when a transfer is pending ("nothing pending" == ok), a REAL failed
        promotion (pending, rewrite failed) still gives ok = false (#175 D1).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
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


def mutate(body: str, old: str, new: str) -> str:
    assert old in body, f"mutation anchor not found: {old!r}"
    return body.replace(old, new, 1)


# ---------------------------------------------------------------------------------------------- #383
def f383(body: str) -> tuple[bool, str]:
    if not re.search(r"bool\s+had_pending\s*=\s*app_xfer_pending\(\)\s*;", body):
        return False, "app_xfer_save_now: had_pending = app_xfer_pending() missing (#383)"
    if not re.search(r"bool\s+promoted\s*=\s*!had_pending\s*\|\|\s*app_xfer_promote\(\)\s*;", body):
        return False, "app_xfer_save_now: promoted must be `!had_pending || app_xfer_promote()` (#383)"
    if not re.search(r"ok\s*=\s*promoted\s*;", body):
        return False, "`ok = promoted;` missing (#175 D1)"
    return True, "ok"


def run() -> None:
    main_t = (SRC / "pdna_main.c").read_text()
    b = function_body(main_t, "app_xfer_save_now")
    check(bool(b), "app_xfer_save_now body not found")
    ok, d = f383(b)
    check(ok, d)
    for label, old, new in (
        ("383: unconditional promote again", "bool promoted = !had_pending || app_xfer_promote();", "bool promoted = app_xfer_promote();"),
        ("383: nothing pending treated as a failure", "!had_pending || app_xfer_promote()", "had_pending && app_xfer_promote()"),
    ):
        if old not in b:
            check(False, f"mutation anchor missing ({label})")
            continue
        ok, _ = f383(mutate(b, old, new))
        check(not ok, f"MUT {label} was NOT caught")


def main() -> int:
    run()
    print(f"host_zz_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
