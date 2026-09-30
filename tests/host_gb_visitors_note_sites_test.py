#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_visitors_note_sites_test.py -- BACKLOG #298 structural guard (pure text, no build).

tests/host_gbdaycare_test.c pins gbd_visitors_note()'s predicate -> wording table. This pins the
CALL SITE: the Gen-1/2 Day-Care panel must feed it the three real app_* predicates in the
function's own argument order (registered, art_off, setting_on), must not fall back to the old
one-line "register a Gen-3 ROM" wording, and pdna_app.h must export the new setting accessor
(no extern global). A swapped argument pair would print the wrong sentence on hardware while the
pure table test stayed green -- that is the gap this closes.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


panel = (ROOT / "source" / "pdna_gbdaycare.c").read_text(encoding="utf-8")
app_h = (ROOT / "source" / "pdna_app.h").read_text(encoding="utf-8")
main_c = (ROOT / "source" / "pdna_main.c").read_text(encoding="utf-8")

call = re.search(r"why\s*=\s*gbd_visitors_note\(\s*(\w+\(\))\s*,\s*(\w+\(\))\s*,\s*(\w+\(\))\s*\)", panel)
check(call is not None, "pdna_gbdaycare.c no longer calls gbd_visitors_note()")
if call:
    check(call.group(1) == "app_any_rom_registered()", f"arg 1 (registered) is {call.group(1)!r}")
    check(call.group(2) == "app_rom_art_off()", f"arg 2 (art_off) is {call.group(2)!r}")
    check(call.group(3) == "app_yard_visitors_setting()", f"arg 3 (setting_on) is {call.group(3)!r}")
check("No visitors: register a Gen-3 ROM" not in panel,
      "the old one-size-fits-all wording is back in pdna_gbdaycare.c")
check(re.search(r"bool\s+app_yard_visitors_setting\s*\(\s*void\s*\)\s*;", app_h) is not None,
      "pdna_app.h does not declare app_yard_visitors_setting()")
check(re.search(r"bool\s+app_yard_visitors_setting\s*\(\s*void\s*\)\s*\{\s*return\s+g_yard_visitors\s*;\s*\}", main_c)
      is not None, "pdna_main.c's app_yard_visitors_setting() does not return g_yard_visitors")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_gb_visitors_note_sites_test: 6 checks passed")
