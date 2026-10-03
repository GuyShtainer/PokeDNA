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

# ---- #402 Settings > Game ROM reaches rom_row_menu for a GB-only owner; app_any_rom_registered keeps its Gen-3 meaning
mainc = rd("pdna_main.c")
pin("#402 Settings branch is GB-aware",
    mainc,
    lambda t: re.search(r"if \(app_any_rom_registered\(\) \|\| app_gb_session_gen\(\) != 0 \|\|\s*app_gb_rom_path\(PDNA_GEN1\)\[0\] \|\| app_gb_rom_path\(PDNA_GEN2\)\[0\]\) rom_row_menu\(\); else app_register_rom\(\);", t) is not None,
    lambda t: t.replace("app_any_rom_registered() || app_gb_session_gen() != 0 ||", "app_any_rom_registered() ||"))
pin("#402 app_any_rom_registered stays Gen-3 only (3 slots)",
    mainc,
    lambda t: re.search(r"bool app_any_rom_registered\(void\) \{\s*for \(int i = 0; i < 3; i\+\+\) if \(g_rom_path\[i\]\[0\]\) return true;\s*return false;", t) is not None,
    lambda t: t.replace("for (int i = 0; i < 3; i++) if (g_rom_path[i][0]) return true;", "for (int i = 0; i < 5; i++) if (g_rom_path[i][0]) return true;"))

# ---- #401 the picker's FULL notice sits right after "n/N" so the 29-col truncation can never eat it
FMT = r'siprintf\(status, "%d/%d  %s%s  %s", g_count \? sel \+ 1 : 0, g_count,\s*g_count >= spec->cap \? "FULL  " : "", sort_label\(\), g_show_all \? "all" : spec->filter_label\);'
pin("#401 FULL precedes the sort label in the browser status line",
    mainc,
    lambda t: re.search(FMT, t) is not None,
    lambda t: t.replace('g_count >= spec->cap ? "FULL  " : "", sort_label()', '"", sort_label()'))
# worst case prefix: 3-digit "n/N" (cap <= 999) + "  FULL" must end inside the 29 visible columns
check_prefix = len("999/999  FULL")
if check_prefix > 29:
    fails.append("FULL no longer fits the 29-col status line")

# ---- #403 (a) art-off wording, (b) cached item-desc pin
lay = rd("pdna_layout.h")
gas = rd("gb_art_source.c")
pin("#403a Extract row says ROM art is off when a ROM is registered but art is off",
    mainc,
    lambda t: "art_off_reg ? PDNA_SET_ART_ARTOFF : PDNA_SET_ART_NEEDROM" in t,
    lambda t: t.replace("art_off_reg ? PDNA_SET_ART_ARTOFF : PDNA_SET_ART_NEEDROM", "PDNA_SET_ART_NEEDROM"))
pin("#403a the A-press refusal names art-off too",
    mainc,
    lambda t: 'off_reg ? "ROM art is off"' in t,
    lambda t: t.replace('off_reg ? "ROM art is off" :', ''))
pin("#403a new Extract value is in the textfit value list",
    lay,
    lambda t: "X(PDNA_SET_ART_ARTOFF)" in t,
    lambda t: t.replace(" X(PDNA_SET_ART_ARTOFF)", ""))
pin("#403b item-desc read re-opens on the cached pin before the full probe",
    gas,
    lambda t: re.search(r"s_idesc\.pin && rom_gbitem_open_pin\([^;]*\)\) \|\|\s*rom_gbitem_open\(", t) is not None,
    lambda t: t.replace("(s_idesc.pin && rom_gbitem_open_pin(", "(0 && rom_gbitem_open_pin("))
pin("#403b the cached pin is dropped at all three session/registration resets",
    gas,
    lambda t: t.count("s_idesc.state = 0; s_idesc.pin = 0;") == 3,
    lambda t: t.replace("s_idesc.state = 0; s_idesc.pin = 0;", "s_idesc.state = 0;", 1))

# ---- #405 dex detail "no art" is centred in the 68-px portrait frame (x 12..79)
pick = rd("pdna_pick.c")
m = re.search(r'ui_text\(st == 0 \? (\d+) : (\d+), 42, UI_DIM, st == 0 \? "\?" : "no art"\);', pick)
if not m:
    fails.append("#405 centred no-art call not found")
else:
    q_x, na_x = int(m.group(1)), int(m.group(2))
    if not (12 <= na_x and na_x + 6 * 8 <= 80 and abs((na_x + 24) - 46) <= 1):
        fails.append("#405 'no art' not centred on the frame (x %d)" % na_x)
    if not (12 <= q_x and q_x + 8 <= 80 and abs((q_x + 4) - 46) <= 1):
        fails.append("#405 '?' not centred on the frame (x %d)" % q_x)
pin("#405 pin is red on the old overrunning position",
    pick,
    lambda t: 'ui_text(38, 42, UI_DIM' not in t,
    lambda t: t.replace('ui_text(st == 0 ? 42 : 22, 42, UI_DIM', 'ui_text(38, 42, UI_DIM'))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_zc_sites_test: all pins green on source and red on mutants")
