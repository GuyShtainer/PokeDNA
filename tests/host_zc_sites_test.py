#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zc_sites_test.py -- lane zc2 structural pins (pure text, no build).

Each pin is checked on the real source and then against an in-memory MUTANT of the same text
(the revert of the fix); a pin that stays green on its mutant is reported as a defect.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
fails = []


def rd(n):
    return (ROOT / "source" / n).read_text(encoding="utf-8")


def pin(name, text, pred, mutate):
    if not pred(text):
        fails.append("RED on real source: " + name)
    m = mutate(text)
    if m == text:
        fails.append("mutant is a no-op (pin cannot be proved): " + name)
    elif pred(m):
        fails.append("pin stays GREEN on its mutant: " + name)


# ---- #400 Gen-2 Day-Care one-boarder panel names the OCCUPIED slot, no NAME (NAME) repeat
dcy = rd("pdna_gbdaycare.c")
pin("#400 n==1 branch picks the occupied slot",
    dcy,
    lambda t: re.search(r"bs\s*=\s*dc->slot\[0\]\.occupied\s*\?\s*&dc->slot\[0\]\s*:\s*&dc->slot\[1\]\s*;\s*[^\n]*\n\s*gbdc_boarder_name\(l,\s*&bs->mon,\s*bs\)", t) is not None,
    lambda t: t.replace("dc->slot[0].occupied ? &dc->slot[0] : &dc->slot[1]", "&dc->slot[0]"))
pin("#400 nickname==species drops the parenthesis",
    dcy,
    lambda t: "if (same || ui_ptext_w(out) > PDNA_DCY_NAME_W)" in t,
    lambda t: t.replace("if (same || ui_ptext_w(out)", "if (ui_ptext_w(out)"))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_zc_sites_test: all pins green on source and red on mutants")
