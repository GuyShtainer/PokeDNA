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
    lambda t: re.search(r'off_reg \? "ROM art is off"\s*:', t) is not None and "const bool off_reg = !s_iconrom.ok && g_rom_art_off && app_any_rom_registered();" in t,
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

# ---- #405 (+#437, romhint F1) dex detail "no art" is centred in the 68-px portrait frame (x 12..79) by MEASURING the proportional
# line; with NO ROM registered the frame carries four centred lines (PDNA_DEX_NOART_L1..L4) in a dark ink that reads on the light gradient
pick = rd("pdna_pick.c")
summ = rd("pdna_summary.c")   # romhint fix2: noart_line moved here (shared by the dex detail and the Gen-3 summary)
lay_h = rd("pdna_layout.h")
def _def(name):
    mm = re.search(r'#define\s+' + name + r'\s+(\d+)\b', lay_h)
    return int(mm.group(1)) if mm else -1
pin("#405 noart_line centres on the frame's own x/width by measuring the line (a fixed x would overrun again)",
    summ,
    lambda t: re.search(r'ui_ptext\(PDNA_DEX_NOART_FX \+ \(PDNA_DEX_NOART_FW - ui_ptext_w\(s\)\) / 2, y, PDNA_DEX_NOART_INK, s\);', t) is not None
              and _def("PDNA_DEX_NOART_FX") == 12 and _def("PDNA_DEX_NOART_FW") == 68,
    lambda t: t.replace('PDNA_DEX_NOART_FX + (PDNA_DEX_NOART_FW - ui_ptext_w(s)) / 2', 'PDNA_DEX_NOART_FX + 26', 1))
pin("romhint F1 the no-art ink is a DARK colour (dim UI ink vanished on the light-blue portrait gradient)",
    lay_h,
    lambda t: (lambda mm: mm is not None and max(int(mm.group(i)) for i in (1, 2, 3)) <= 8)(
        re.search(r'#define PDNA_DEX_NOART_INK\s+RGB15\((\d+),\s*(\d+),\s*(\d+)\)', t)),
    lambda t: t.replace('PDNA_DEX_NOART_INK   RGB15(1, 2, 6)', 'PDNA_DEX_NOART_INK   RGB15(17, 18, 21)', 1))
pin("romhint F1 no-art lines are drawn by pdna_summary_noart_line (the dark ink), never ui_text(UI_DIM)",
    summ,
    lambda t: all(re.search(x, t) for x in (r'pdna_summary_noart_line\(42, PDNA_DEX_NOART_L1\)',
                                            r'pdna_summary_noart_line\(26, PDNA_DEX_NOART_L1\)', r'pdna_summary_noart_line\(36, PDNA_DEX_NOART_L2\)',
                                            r'pdna_summary_noart_line\(46, PDNA_DEX_NOART_L3\)', r'pdna_summary_noart_line\(56, PDNA_DEX_NOART_L4\)')),
    lambda t: t.replace('pdna_summary_noart_line(36, PDNA_DEX_NOART_L2)', 'ui_text(14, 42, UI_DIM, PDNA_DEX_NOART_L2)', 1))
pin("romhint fix2 #5 the dex detail AND the Gen-3 summary draw the same pointer (dex: '?' for unseen, else pdna_summary_noart_pointer)",
    pick,
    lambda t: 'pdna_summary_noart_line(42, "?")' in t and "else pdna_summary_noart_pointer();" in t and "static void noart_line" not in t
              and "else pdna_summary_noart_pointer();   /* romhint fix2 #5" in summ,
    lambda t: t.replace("else pdna_summary_noart_pointer();", "else (void)0;", 1))
pin("romhint F1 the pointer names the WHOLE term (L3 + L4 rebuild PDNA_ROM_WHERE: Settings > Game ROM)",
    lay_h,
    lambda t: re.search(r'PDNA_DEX_NOART_L3\s+PDNA_ROM_WHERE_A', t) is not None
              and re.search(r'PDNA_DEX_NOART_L4\s+PDNA_ROM_WHERE_B', t) is not None
              and re.search(r'#define PDNA_ROM_WHERE PDNA_ROM_WHERE_A " " PDNA_ROM_WHERE_B', t) is not None,
    lambda t: t.replace('#define PDNA_DEX_NOART_L4 PDNA_ROM_WHERE_B', '#define PDNA_DEX_NOART_L4 ""', 1))
pin("#437 the no-art pointer is gated on 'no ROM registered' (a registered ROM keeps plain 'no art')",
    summ,
    lambda t: re.search(r'if \(app_any_rom_registered\(\) \|\| app_gb_rom_path\(PDNA_GEN1\)\[0\] \|\| app_gb_rom_path\(PDNA_GEN2\)\[0\]\) \{\s*pdna_summary_noart_line\(42, PDNA_DEX_NOART_L1\);', t) is not None,
    lambda t: t.replace('if (app_any_rom_registered() ||', 'if (0 ||', 1))

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
