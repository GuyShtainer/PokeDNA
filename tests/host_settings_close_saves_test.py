#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_settings_close_saves_test.py -- BACKLOG #309 structural pin (pure text, no build).

Settings exits two ways: B (which has always called cfg_save()) and the Close row (S_CLOSE, the
`else` arm at the end of the A-handler). Close returned without saving, so a History size / backup
mode / yard switch changed on the screen was lost on power-off unless something else wrote
config.cfg later. This pins that EVERY `return` out of the settings loop (the B key and the S_CLOSE
arm) is preceded by cfg_save() in the same block.

It is text-level on purpose: the save is an SD write, which no host build reaches. What it does NOT
prove is that config.cfg really carries the value across a power cycle -- that is hardware/vsd work.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
src = (ROOT / "source" / "pdna_main.c").read_text(encoding="utf-8")
fails = []

i = src.index("enum { S_BACKUP, S_ANIM, S_YARD, S_ROM, S_ART, S_RUMBLE, S_HIST, S_HCLEAR, S_CLEAR, S_CLOSE, S_N };")
body = src[i:i + 9000]
end = body.index("/* S_CLOSE")
loop = body[:end + 400]

# the S_CLOSE arm: `else { cfg_save(); return; }   /* S_CLOSE`
m = re.search(r"else\s*\{\s*cfg_save\(\);\s*return;\s*\}\s*/\*\s*S_CLOSE", loop)
if not m:
    fails.append("the S_CLOSE arm does not call cfg_save() before returning")
# the B arm of the same loop
if not re.search(r"else if \(k & KEY_B\)\s*\{\s*cfg_save\(\);\s*return;\s*\}", loop):
    fails.append("the settings B key no longer calls cfg_save() before returning")
# no bare `return;` arm left (an `else return;` / `{ return; }` without a save)
for bad in re.finditer(r"else\s+return;|KEY_B\)\s*\{\s*return;", loop):
    fails.append("a settings exit returns without cfg_save(): " + loop[bad.start():bad.end() + 20].strip())

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_settings_close_saves_test: 3 checks passed")
