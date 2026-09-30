#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_bank_move_step_name_test.py -- BACKLOG #306 structural pin (pure text, no build).

A drop that CROSSES into another file (Bank -> PC inject, PC -> Bank release) shares the plain drop's
"Box move" scope (pdna_box.c opens it at the two A-handlers before calling drop_held), so the history
row and the floor refusal ("Can't undo Box move. It crossed into another file") named the wrong thing.
The fix renames the open scope at the cross site with app_step_name("Bank move"). This pins:

  (a) both crossing branches of drop_held() call app_step_name("Bank move") BEFORE their cross call
      (app_bank_defer_delete for Bank -> PC, app_pc_release_slot for PC -> Bank);
  (b) the two plain-drop scopes are still opened as "Box move" (the name a plain move keeps);
  (c) "Bank move" is <= 24 ASCII chars (the journal's step-name limit, pdna_app.h).

tests/host_jrn_funnel_test.c pins the PURE half: the renamed scope's recorded step is "Bank move" and
the rename does not leak into the next scope.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
src = (ROOT / "source" / "pdna_box.c").read_text(encoding="utf-8")
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


for cross in ("app_bank_defer_delete", "app_pc_release_slot"):
    m = re.search(r'app_step_name\("Bank move"\);(?:\s|/\*.*?\*/)*' + cross + r"\(", src)
    check(m is not None, f'no app_step_name("Bank move") directly before {cross}() in drop_held')
check(len(re.findall(r'app_step_begin\("Box move"\)', src)) == 2,
      'the two plain-drop scopes are no longer both opened as "Box move"')
check(len(re.findall(r'app_step_name\("Bank move"\)', src)) == 2,
      'expected exactly two "Bank move" rename sites (Bank->PC, PC->Bank)')
check(len("Bank move") <= 24, "step name too long")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_bank_move_step_name_test: 4 checks passed")
