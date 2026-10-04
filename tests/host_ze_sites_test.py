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

# ---- #411 FULL means "an entry past the cap exists" (scan_dir lookahead), not "count reached the cap"
pin("#411 scan_dir admits through scan_cap_admit AFTER the filters (the lookahead sees only passing entries)",
    main, lambda t: re.search(r"spec_ext_match\(spec, fno\.fname\)\) continue;[^\n]*\n\s*if \(!scan_cap_admit\(g_count, spec->cap, &g_scan_more\)\) break;", t) is not None,
    lambda t: t.replace("if (!scan_cap_admit(g_count, spec->cap, &g_scan_more)) break;", ""))
pin("#411 the readdir loop no longer stops AT the cap (the lookahead must read one entry past it)",
    main, lambda t: "while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {" in t and "while (g_count < spec->cap && f_readdir" not in t,
    lambda t: t.replace("while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {", "while (g_count < spec->cap && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {"))
pin("#411 the status line's FULL comes from g_scan_more",
    main, lambda t: 'g_scan_more ? "FULL  " : ""' in t,
    lambda t: t.replace('g_scan_more ? "FULL  " : ""', 'g_count >= spec->cap ? "FULL  " : ""'))
pin("#411 g_scan_more is cleared at every scan start",
    main, lambda t: re.search(r"g_count = 0;\s*g_scan_more = 0;", t) is not None,
    lambda t: t.replace("g_scan_more = 0;\n  DIR dir;", "DIR dir;"))
pin("#411 g_scan_more is EWRAM_BSS",
    main, lambda t: "EWRAM_BSS g_scan_more;" in t,
    lambda t: t.replace("EWRAM_BSS g_scan_more;", "g_scan_more;"))

# ---- #412 Game ROM row: a Gen 1/2 path with no Gen-3 ROM reads "GB only", not "not set"
pin("#412 the Game ROM row falls to PDNA_SET_ROM_GBONLY when a GB ROM path (gen 1 or 2) is registered",
    main, lambda t: re.search(r"app_gb_rom_path\(PDNA_GEN1\)\[0\] \|\| app_gb_rom_path\(PDNA_GEN2\)\[0\]\)\s*\? PDNA_SET_ROM_GBONLY : PDNA_SET_ROM_NOTSET", t) is not None,
    lambda t: t.replace("? PDNA_SET_ROM_GBONLY : PDNA_SET_ROM_NOTSET", "? PDNA_SET_ROM_NOTSET : PDNA_SET_ROM_NOTSET"))
pin("#412 app_any_rom_registered keeps its Gen-3-only meaning (slots 0-2)",
    main, lambda t: "for (int i = 0; i < 3; i++) if (g_rom_path[i][0]) return true;" in t,
    lambda t: t.replace("for (int i = 0; i < 3; i++) if (g_rom_path[i][0])", "for (int i = 0; i < APP_ROM_SLOTS; i++) if (g_rom_path[i][0])"))
lay = rd("pdna_layout.h")
pin("#412 GBONLY is in the textfit value list",
    lay, lambda t: "X(PDNA_SET_ROM_GBONLY)" in t,
    lambda t: t.replace("X(PDNA_SET_ROM_GBONLY) ", ""))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_ze_sites_test: all pins green on source and red on mutants")
