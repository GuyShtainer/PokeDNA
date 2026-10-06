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
pin("#400 the nickname==species test is a real case-blind equality (review-zc2 F1)",
    dcy,
    lambda t: re.search(r"if \(a != b\) same = false;\s*\n\s*if \(a == 0 \|\| b == 0\) break;", t) is not None,
    lambda t: t.replace("if (a != b) same = false;", "/* if (a != b) same = false; */"))

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
FMT = r'siprintf\(status, "%d/%d  %s%s  %s", g_count \? sel \+ 1 : 0, g_count,\s*g_scan_more \? "FULL  " : "", sort_label\(\), g_show_all \? "all" : spec->filter_label\);'
pin("#401 FULL precedes the sort label in the browser status line",
    mainc,
    lambda t: re.search(FMT, t) is not None,
    lambda t: t.replace('g_scan_more ? "FULL  " : "", sort_label()', '"", sort_label()'))   # #411: the flag, not "count reached cap"
# worst case prefix: 3-digit "n/N" (cap <= 999) + "  FULL" must end inside the 29 visible columns
check_prefix = len("999/999  FULL")
if check_prefix > 29:
    fails.append("FULL no longer fits the 29-col status line")

# ---- #403 (a) art-off wording, (b) cached item-desc pin
lay = rd("pdna_layout.h")
gas = rd("gb_art_source.c")
pin("#403a Extract row says ROM art is off when a ROM is registered but art is off",
    mainc,
    lambda t: re.search(r"art_off_reg \? PDNA_SET_ART_ARTOFF\s*:\s*app_any_rom_registered\(\) \? PDNA_SET_ART_MAPONLY[^\n]*\n\s*: PDNA_SET_ART_NEEDROM", t) is not None,   # #439 inserted the map-only rung between the two
    lambda t: t.replace("art_off_reg ? PDNA_SET_ART_ARTOFF", "0 ? PDNA_SET_ART_ARTOFF"))
pin("#403a the A-press refusal names art-off too",
    mainc,
    lambda t: re.search(r'off_reg \? "ROM art is off"\s*:', t) is not None,
    lambda t: re.sub(r'off_reg \? "ROM art is off"\s*:', '', t, count=1))
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

# ---- #405 (+#437) dex detail "no art" is centred in the 68-px portrait frame (x 12..79); with NO ROM registered the
# frame carries three centred 8-px-glyph lines ("no art" / PDNA_DEX_NOART_L2 / PDNA_DEX_NOART_L3)
pick = rd("pdna_pick.c")
lay_h = rd("pdna_layout.h")
def _noart_xs(t):
    q = re.search(r'ui_text\((\d+), 42, UI_DIM, "\?"\);', t)
    reg = re.search(r'ui_text\((\d+), 42, UI_DIM, "no art"\);', t)
    l1 = re.search(r'ui_text\((\d+), 32, UI_DIM, "no art"\);', t)
    l2 = re.search(r'ui_text\((\d+), 42, UI_DIM, PDNA_DEX_NOART_L2\);', t)
    l3 = re.search(r'ui_text\((\d+), 52, UI_DIM, PDNA_DEX_NOART_L3\);', t)
    return q, reg, l1, l2, l3
def _macro_len(name):
    mm = re.search(r'#define\s+' + name + r'\s+"([^"]*)"', lay_h)
    return len(mm.group(1)) if mm else -1
q, reg, l1, l2, l3 = _noart_xs(pick)
if not all((q, reg, l1, l2, l3)):
    fails.append("#405/#437 centred no-art calls not found")
else:
    def centred(x, glyphs, what):
        if not (12 <= x and x + glyphs * 8 <= 80 and abs((x + glyphs * 4) - 46) <= 1):
            fails.append("#405/#437 %s not centred on the frame (x %d, %d glyphs)" % (what, x, glyphs))
    centred(int(q.group(1)), 1, "'?'")
    centred(int(reg.group(1)), 6, "'no art' (ROM registered)")
    centred(int(l1.group(1)), 6, "'no art' (no ROM, line 1)")
    centred(int(l2.group(1)), _macro_len("PDNA_DEX_NOART_L2"), "PDNA_DEX_NOART_L2")
    centred(int(l3.group(1)), _macro_len("PDNA_DEX_NOART_L3"), "PDNA_DEX_NOART_L3")
pin("#405 pin is red on the old overrunning position",
    pick,
    lambda t: 'ui_text(38, 42, UI_DIM' not in t,
    lambda t: t.replace('ui_text(22, 42, UI_DIM, "no art")', 'ui_text(38, 42, UI_DIM, "no art")'))
pin("#437 the pointer lines are on the frame's centre (a shifted line is caught)",
    pick,
    lambda t: all(_noart_xs(t)) and abs((int(_noart_xs(t)[3].group(1)) + _macro_len("PDNA_DEX_NOART_L2") * 4) - 46) <= 1,
    lambda t: t.replace('ui_text(14, 42, UI_DIM, PDNA_DEX_NOART_L2)', 'ui_text(38, 42, UI_DIM, PDNA_DEX_NOART_L2)'))
pin("#437 the no-art pointer is gated on 'no ROM registered' (a registered ROM keeps plain 'no art')",
    pick,
    lambda t: re.search(r'else if \(app_any_rom_registered\(\) \|\| app_gb_rom_path\(PDNA_GEN1\)\[0\] \|\| app_gb_rom_path\(PDNA_GEN2\)\[0\]\) ui_text\(22, 42, UI_DIM, "no art"\);', t) is not None,
    lambda t: t.replace('else if (app_any_rom_registered() ||', 'else if (0 ||'))

# ---- #409 pick_rows footer is drawn through ui_ptext_fit from the shared layout macros
pin("#409 pick_rows footer uses the proportional fitted draw",
    pick,
    lambda t: "ui_ptext_fit(PDNA_PICKROWS_FOOT_X, 152, PDNA_PICKROWS_FOOT_W, UI_DIM, foot)" in t,
    lambda t: t.replace("ui_ptext_fit(PDNA_PICKROWS_FOOT_X, 152, PDNA_PICKROWS_FOOT_W, UI_DIM, foot)", "ui_text(4, 152, UI_DIM, foot)"))

# ---- #399 both CREATE-edit footers end "START keep" (the Gen-3 and Game Boy twins share one verb)
pin("#399 Gen-3 CREATE-edit footer names START's verb",
    lay,
    lambda t: re.search(r'PDNA_SUM_FOOT_CREATE_EDIT\s+"[^"]*START keep"', t) is not None,
    lambda t: t.replace('L/R START keep"', 'L/R START"'))
pin("#399 Game Boy CREATE-edit footer names START's verb",
    lay,
    lambda t: re.search(r'PDNA_GBSUM_FOOT_CREATE_EDIT\s+"[^"]*START keep"', t) is not None,
    lambda t: t.replace('L/R START keep"', 'L/R START"'))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_zc_sites_test: all pins green on source and red on mutants")
