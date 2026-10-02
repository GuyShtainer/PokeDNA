#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_zw_sites_test.py -- lane zw (BACKLOG #365-#369): wiring pins with teeth.

Each pin is a text check over comment-stripped function bodies; every pin is re-run against a MUTANT copy of
the real source (the fix reverted) and must FAIL there:
  Z367 gbdc_land: on Gen 1 the party leg uses gbs_insert_party() with a ROM base row, never gbs_move() box->party
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
fails: list[str] = []
n_checks = 0


def check(name, ok, detail=""):
    global n_checks
    n_checks += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"  [{detail}]"))
    if not ok:
        fails.append(name)


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def body(text, fn):
    t = strip_comments(text)
    m = re.search(r"(?m)^[A-Za-z_][^;{}\n]*\b" + re.escape(fn) + r"\s*\([^;{]*\)\s*\{", t)
    if not m:
        return ""
    i, d = m.end() - 1, 0
    for j in range(i, len(t)):
        d += (t[j] == "{") - (t[j] == "}")
        if d == 0:
            return t[i:j + 1]
    return ""


def rd(n):
    return (SRC / n).read_text(errors="replace")


# ---- pins: name -> (file, function, predicate over the body) -----------------------------------------------
def p367(b):
    g1 = b.find("GB_GEN1")
    ip = b.find("gbs_insert_party(")
    mv = b.find("gbs_move(")
    return (g1 >= 0 and 0 <= ip < mv and g1 < ip and "gb12_gen1_base_for(" in b
            and "GBS_ERR_NEEDS_BASE" in b)


PINS = [("Z367 Gen-1 Day-Care party leg = gbs_insert_party + ROM base row", "pdna_gbdaycare.c", "gbdc_land", p367)]

# mutants: (pin name, file, old, new) -- applied to the REAL source text, the pin must go RED
MUTANTS = [
    ("Z367 Gen-1 Day-Care party leg = gbs_insert_party + ROM base row", "pdna_gbdaycare.c",
     "gbs_insert_party(s, mon, &g1base, &pslot, list)", "gbs_insert(s, 0, mon, &pslot, list)"),
    ("Z367 Gen-1 Day-Care party leg = gbs_insert_party + ROM base row", "pdna_gbdaycare.c",
     "if (s->gen == GB_GEN1) {\n      /* #367", "if (0) {\n      /* #367"),
]


def main():
    srcs = {}
    for name, f, fn, pred in PINS:
        srcs.setdefault(f, rd(f))
        b = body(srcs[f], fn)
        check(name, bool(b) and pred(b), "body missing" if not b else "predicate false")
    for name, f, old, new in MUTANTS:
        pin = next(p for p in PINS if p[0] == name)
        t = srcs[f]
        check(f"mutant applies: {old[:40]!r}", old in t)
        b = body(t.replace(old, new, 1), pin[2])
        check(f"mutant RED: {name} / {new[:30]!r}", not (b and pin[3](b)))
    print(f"{n_checks} checks, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
