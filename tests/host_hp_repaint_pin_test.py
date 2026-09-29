#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_hp_repaint_pin_test.py -- BACKLOG #231 (y8 G3): current HP is a SEPARATE repaint input.

Current HP lives outside PkMon's compared bytes on the Gen-3 cards, so both incremental
repaint paths compare it explicitly. Dropping either compare leaves stale pixels when only
the HP changed (the y8 reviewer's mut_hp_rows.png). Two structural pins, each self-tested
with a mutant that must go RED:

  1. source/pdna_summary.c card_paint_needed() must contain `v->hp != hp`.
  2. source/pdna_edit.c render() must compare each row against the shadow's own HP:
     `field_value(f, &pv->mon, pv->hp, b)`, and must store `pv->hp = hp`.

    python3 tests/host_hp_repaint_pin_test.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "source"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body_of(text: str, header: str) -> str:
    """Body of the function whose signature line starts with `header` (brace matched)."""
    start = text.find(header)
    assert start >= 0, f"{header!r} not found"
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        depth += (text[j] == "{") - (text[j] == "}")
        if depth == 0:
            return text[start:j + 1]
    raise AssertionError("unbalanced braces")


def check_summary(text: str) -> list[str]:
    body = strip_comments(body_of(text, "static bool card_paint_needed("))
    ok = re.search(r"v->hp\s*!=\s*hp\b", body) is not None
    return [] if ok else ["card_paint_needed() no longer compares v->hp against hp"]


def check_edit(text: str) -> list[str]:
    body = strip_comments(body_of(text, "static void render(const PkMon* c, uint16_t hp"))
    errs = []
    if not re.search(r"field_value\(\s*f\s*,\s*&pv->mon\s*,\s*pv->hp\s*,", body):
        errs.append("render() no longer compares the row against the shadow's own pv->hp")
    if not re.search(r"pv->hp\s*=\s*hp\s*;", body):
        errs.append("render() no longer stores pv->hp = hp")
    return errs


def main() -> int:
    summary = (SRC / "pdna_summary.c").read_text()
    edit = (SRC / "pdna_edit.c").read_text()
    errs = check_summary(summary) + check_edit(edit)
    # mutants: the compare deleted / made stale must go RED
    m1 = summary.replace("|| v->hp   != hp\n", "", 1)
    m2 = edit.replace("field_value(f, &pv->mon, pv->hp, b);", "field_value(f, &pv->mon, hp, b);", 1)
    m3 = edit.replace("pv->hp  = hp;", "", 1)
    for name, mut, orig, fn in (("summary compare deleted", m1, summary, check_summary),
                                ("edit shadow compare stale", m2, edit, check_edit),
                                ("edit shadow store deleted", m3, edit, check_edit)):
        if mut == orig:
            errs.append(f"SELF-TEST {name}: mutation did not apply")
        elif not fn(mut):
            errs.append(f"SELF-TEST {name}: the pin did not catch it")
    for e in errs:
        print("  !! FAIL:", e)
    print("ok" if not errs else "FAIL")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
