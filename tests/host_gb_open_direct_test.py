#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_open_direct_test.py -- BACKLOG #279: a Gen-1/2 save opens straight into the box grid.

gb_session_core() (source/pdna_gen12.c) is the ONE place every GB mount (resident image,
streamed FIL, fused cart) enters the box grid. It used to open with

    if (!gb_info_page(m)) return;      // "GAME BOY SAVE" page, A browse / B back

which Gen 3 never has. Guy: "it should simply enter it". This structural pin reads the
function body and requires that no gb_info_page( CALL precedes the box-grid loop
(`pdna_box(&s)`), so the page cannot creep back in front of the grid. The page itself stays
(the no-session nav fallbacks call it).

    python3 tests/host_gb_open_direct_test.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "source" / "pdna_gen12.c"


def core_body(text: str) -> str:
    m = re.search(r"static void gb_session_core\(Gb12Mount\* m, GbSession\* ro\) \{", text)
    assert m, "gb_session_core not found"
    depth, i = 0, m.end() - 1
    while True:
        c = text[i]
        depth += (c == "{") - (c == "}")
        i += 1
        if depth == 0:
            return text[m.start():i]


def check(text: str) -> list[str]:
    body = core_body(text)
    # strip comments so prose mentioning the page is not mistaken for a call
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    grid = code.find("pdna_box(&s)")
    bad = [m.start() for m in re.finditer(r"\bgb_info_page\s*\(", code)]
    errs = []
    if grid < 0:
        errs.append("the box-grid loop (pdna_box(&s)) is missing from gb_session_core")
    if any(b < grid for b in bad):
        errs.append("gb_info_page() is called before the box grid in gb_session_core (#279 regression)")
    return errs


def main() -> int:
    text = SRC.read_text()
    errs = check(text)
    # mutant: put the old line back at the top of the body -- the check must go RED
    anchor = "static void gb_session_core(Gb12Mount* m, GbSession* ro) {\n"
    mutant = text.replace(anchor, anchor + "  if (!gb_info_page(m)) return;\n", 1)
    assert mutant != text, "mutation did not apply"
    if not check(mutant):
        errs.append("SELF-TEST: the pin did not catch the re-inserted gb_info_page gate")
    for e in errs:
        print("  !! FAIL:", e)
    print("ok" if not errs else "FAIL")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
