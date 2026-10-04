#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_ze_sites_test.py -- lane ze structural pins (pure text, no build).

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


main = rd("pdna_main.c")
# ---- #396 the ROM-hack banner wait is outside the `perf boot` span
RH = (r"hb_pause\(\); perf_span_pause\(\);\s*msg_wait\(PDNA_ROMHACK_TITLE, UI_WARN, PDNA_ROMHACK_L1, PDNA_ROMHACK_L2\);\s*"
      r"perf_span_resume\(\); hb_resume\(\);")
pin("#396 the ROM-hack banner msg_wait is bracketed by perf_span_pause/resume (like save_slot_warn)",
    main, lambda t: re.search(RH, t) is not None,
    lambda t: t.replace("perf_span_resume(); hb_resume();\n  } else {\n    app_src_readonly_clear();", "} else {\n    app_src_readonly_clear();"))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_ze_sites_test: all pins green on source and red on mutants")
