#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove tests/host_textfit_test.c can actually FAIL.  python3 tests/textfit_mutation_check.py

A layout test that re-types the screens' numbers as its own literals passes forever: it
measures its own copy. That happened here, and the fix was to move every asserted number
and fixed string into headers the shipped code reads (source/ui_layout.h,
source/pdna_layout.h, source/rmbl.h). This script is the proof that the wiring is real.

For every (file, old, new) below it patches the SHIPPED definition, rebuilds + runs the
text-fit test, records whether it went red and which check bit, then restores the file
byte-for-byte. A perturbation that leaves the test GREEN is a vacuous check: either wire
that constant up or delete the check. Exit status is 0 only if all of them bite.

It edits source files in place, so do not run it with unsaved work in those three headers;
every mutation is restored in a finally, including on Ctrl-C.
"""
import subprocess, sys, os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
os.chdir(ROOT)

UL = "source/ui_layout.h"
PL = "source/pdna_layout.h"
RM = "source/rmbl.h"

M = [
 # --- ui_layout.h: the screen frame + the real popup layout function ---------
 (UL, "#define UI_FOOTER_Y  150", "#define UI_FOOTER_Y  140", "UI_FOOTER_Y 150->140"),
 (UL, "#define UI_SCR_W   240", "#define UI_SCR_W   200", "UI_SCR_W 240->200"),
 (UL, "#define UI_ROW_H     8", "#define UI_ROW_H     10", "UI_ROW_H 8->10"),
 (UL, "  int y = (UI_FOOTER_Y - h) / 2;", "  int y = (UI_SCR_H - h) / 2;",
      "ui_popup_fit centres on the SCREEN again (the old bug)"),
 (UL, "  if (vis > nrows) vis = nrows;", "  if (vis > nrows + 4) vis = nrows;",
      "ui_popup_fit stops clamping vis to nrows"),
 # --- nav menu ---------------------------------------------------------------
 (PL, "#define PDNA_NAV_ROW_H   11", "#define PDNA_NAV_ROW_H   13", "NAV_ROW_H 11->13 (the shipped regression)"),
 (PL, "#define PDNA_NAV_HEAD    20", "#define PDNA_NAV_HEAD    30", "NAV_HEAD 20->30"),
 (PL, "#define PDNA_NAV_FOOT    14", "#define PDNA_NAV_FOOT    24", "NAV_FOOT 14->24"),
 (PL, "#define PDNA_NAV_COL_W   98", "#define PDNA_NAV_COL_W   80", "NAV_COL_W 98->80 (narrower band)"),
 (PL, "#define PDNA_NAV_LABEL_DX 4", "#define PDNA_NAV_LABEL_DX 20", "NAV_LABEL_DX 4->20"),
 (PL, "#define PDNA_NAV_HINT_DY (-10)", "#define PDNA_NAV_HINT_DY (-3)", "NAV_HINT_DY -10->-3"),
 (PL, '#define PDNA_NAV_HINT    "A pick  B back"',
      '#define PDNA_NAV_HINT    "A pick  B back  START close"', "NAV_HINT string grows"),
 (PL, 'X(NV_DATA,       "Flags & counters")', 'X(NV_DATA,       "Flags & counters and more")',
      'nav label "Flags & counters" grows'),
 (PL, '  X(NV_BACK,       "Back")', '  X(NV_BACK,       "Back")           \\\n  X(NV_T1, "T1") X(NV_T2, "T2")',
      "two more nav entries (20 -> 21)"),
 # --- per-mon action popup ---------------------------------------------------
 (PL, "#define PDNA_MONMENU_W     100", "#define PDNA_MONMENU_W     90", "MONMENU_W 100->90"),
 (PL, "#define PDNA_MONMENU_ROW_DX  6", "#define PDNA_MONMENU_ROW_DX 14", "MONMENU_ROW_DX 6->14"),
 (PL, '#define PDNA_LBL_MOVE_TO_BOX "MOVE TO BOX"', '#define PDNA_LBL_MOVE_TO_BOX "MOVE TO A BOX"',
      'action label "MOVE TO BOX" grows'),
 (PL, '#define PDNA_MONMENU_FOOT_TXT "A ok B back"', '#define PDNA_MONMENU_FOOT_TXT "A ok  B back"',
      'popup hint "A ok B back" grows by one space'),
 (PL, "#define PDNA_MONMENU_HEAD   18", "#define PDNA_MONMENU_HEAD   130", "MONMENU_HEAD 18->130 (no room for a row)"),
 (PL, "#define PDNA_MONMENU_ROW_H  13", "#define PDNA_MONMENU_ROW_H  130", "MONMENU_ROW_H 13->130"),
 (PL, "#define PDNA_ROMENU_LINE      10", "#define PDNA_ROMENU_LINE      60", "ROMENU_LINE 10->60"),
 (PL, "#define PDNA_DEXBULK_HEAD   18", "#define PDNA_DEXBULK_HEAD   130", "DEXBULK_HEAD 18->130"),
 (PL, "#define PDNA_DCPOP_FOOT      8", "#define PDNA_DCPOP_FOOT      140", "DCPOP_FOOT 8->140"),
 (PL, "#define PDNA_POPUP_HINT_DY   (-9)", "#define PDNA_POPUP_HINT_DY   (-8)", "POPUP_HINT_DY -9->-8"),
 # --- FILTER / SORT lists ----------------------------------------------------
 (PL, "#define PDNA_FILT_ROW_H      9", "#define PDNA_FILT_ROW_H      8", "FILT_ROW_H 9->8 (the shipped regression)"),
 (PL, "#define PDNA_FILT_BAR_H      9", "#define PDNA_FILT_BAR_H      8", "FILT_BAR_H 9->8"),
 (PL, "#define PDNA_FILT_BAR_DY   (-1)", "#define PDNA_FILT_BAR_DY   (1)", "FILT_BAR_DY -1->+1"),
 (PL, "#define PDNA_FILT_VIS       15", "#define PDNA_FILT_VIS       16", "FILT_VIS 15->16"),
 (PL, "#define PDNA_FILT_Y0        14", "#define PDNA_FILT_Y0        20", "FILT_Y0 14->20"),
 (PL, "#define PDNA_IFILT_BOX_H    12", "#define PDNA_IFILT_BOX_H    11", "IFILT_BOX_H 12->11"),
 (PL, "#define PDNA_IFILT_ROW_H    11", "#define PDNA_IFILT_ROW_H    9", "IFILT_ROW_H 11->9"),
 (PL, "#define PDNA_IFILT_NCAT      6", "#define PDNA_IFILT_NCAT      12", "IFILT_NCAT 6->12 (six new item categories)"),
 # --- settings page ----------------------------------------------------------
 (PL, "#define PDNA_SET_ROW_X        10", "#define PDNA_SET_ROW_X        20", "SET_ROW_X 10->20"),
 (PL, "#define PDNA_SET_HELP_X        8", "#define PDNA_SET_HELP_X        40", "SET_HELP_X 8->40"),
 (PL, '#define PDNA_SET_YARD_NEEDROM "Set Game ROM"', '#define PDNA_SET_YARD_NEEDROM "Needs your ROM"',
      'yard value back to "Needs your ROM" (the shipped wrap bug)'),
 (PL, '#define PDNA_SET_BACKUP_FMT  "Backups:  %s"', '#define PDNA_SET_BACKUP_FMT  "Backup mode:  %s"',
      "backup row FORMAT grows"),
 (PL, '#define PDNA_SET_ROW_CLEAR   "Clear backups (this save)"',
      '#define PDNA_SET_ROW_CLEAR   "Clear all backups (this save)"', '"Clear backups" row grows'),
 (PL, '#define PDNA_SET_HELP1       "Animations + Rumble have"',
      '#define PDNA_SET_HELP1       "Animations and Rumble both have"', "settings help line 1 grows"),
 (PL, '#define PDNA_SET_HELP2       "per-item on/off submenus."',
      '#define PDNA_SET_HELP2       "per-item on and off submenus."', "settings help line 2 grows"),
 (PL, '#define PDNA_SET_FOOT        "A change/do  U/D move  B back"',
      '#define PDNA_SET_FOOT        "A change/do  U/D move  B go back"', "settings footer grows"),
 (PL, '#define PDNA_SET_NOTE        "Yard visitors are scenery, not your Pokemon."',
      '#define PDNA_SET_NOTE        "Yard visitors are only scenery, not your own Pokemon."',
      "yard-visitors note grows"),
 (PL, "#define PDNA_SET_ROWS          7", "#define PDNA_SET_ROWS          9", "SET_ROWS 7->9 (two new settings)"),
 (PL, "#define PDNA_SET_NOTE_Y      142", "#define PDNA_SET_NOTE_Y      146", "SET_NOTE_Y 142->146"),
 # --- rumble page ------------------------------------------------------------
 (PL, "#define PDNA_RMB_HELP_Y1     132", "#define PDNA_RMB_HELP_Y1     110", "RMB_HELP_Y1 132->110"),
 (PL, "#define PDNA_RMB_HELP_Y2     141", "#define PDNA_RMB_HELP_Y2     146", "RMB_HELP_Y2 141->146"),
 (PL, '#define PDNA_RMB_HELP1       "Needs an EZ-Flash Omega"',
      '#define PDNA_RMB_HELP1       "Needs an EZ-Flash Omega DE cart"', "rumble help line 1 grows"),
 (PL, '#define PDNA_RMB_HELP2       "with GAME RTC on."',
      '#define PDNA_RMB_HELP2       "with the GAME RTC option switched on."', "rumble help line 2 grows"),
 (PL, '#define PDNA_RMB_FOOT        "<> adjust  A toggle  B back"',
      '#define PDNA_RMB_FOOT        "<> adjust  A toggle  B go back"', "rumble footer grows"),
 (RM, "       RCUE_COUNT };", "       RCUE_TEST1, RCUE_TEST2,\n       RCUE_COUNT };",
      "two more haptic cues (RCUE_COUNT 5->7)"),
 # --- day-care yard panel ----------------------------------------------------
 (PL, "#define PDNA_DCY_PANEL_H    30", "#define PDNA_DCY_PANEL_H    28", "DCY_PANEL_H 30->28 (the shipped shear)"),
 (PL, "#define PDNA_DCY_ROW0_Y    124", "#define PDNA_DCY_ROW0_Y    125", "DCY_ROW0_Y 124->125 (the old rows)"),
 (PL, "#define PDNA_DCY_ROW_PITCH   9", "#define PDNA_DCY_ROW_PITCH   10", "DCY_ROW_PITCH 9->10"),
 (PL, "#define PDNA_DCY_PANEL_W   236", "#define PDNA_DCY_PANEL_W   200", "DCY_PANEL_W 236->200"),
 (PL, "#define PDNA_DCY_FOOTER_Y  152", "#define PDNA_DCY_FOOTER_Y  150", "DCY_FOOTER_Y 152->150"),
 (PL, "#define PDNA_DCY_NAME_W    228", "#define PDNA_DCY_NAME_W    236", "DCY_NAME_W 228->236"),
 (PL, '#define PDNA_DCY_HINT_PAIR "A menu  L/R your 2  B back"',
      '#define PDNA_DCY_HINT_PAIR "A menu  L/R your own 2 of them  B go back please"', "day-care hint grows"),
 # --- 2026-08: the guards a verifier proved could not fail -------------------
 # (1) UI_FOOTER_Y is now the y the screens DRAW their footer at, so it has to be a
 #     legal draw position in both directions, not only a ceiling popups stay under.
 (UL, "#define UI_FOOTER_Y  150", "#define UI_FOOTER_Y  153",
      "UI_FOOTER_Y 150->153 (footer band rises above a real footer)"),
 (UL, "#define UI_FOOTER_Y  150", "#define UI_FOOTER_Y  156",
      "UI_FOOTER_Y 150->156 (footer ink runs off the bottom of the screen)"),
 # (2) ui_popup_fit's clamps. Every SHIPPED geometry lands in the same branch, so these
 #     bite only because the contract sweep drives the function at hostile inputs.
 (UL, "  if (vis < 1)     vis = 1;", "  /* clamp removed */",
      "ui_popup_fit drops its 'at least one row' clamp"),
 (UL, "  if (y < 0) y = 0;", "  /* clamp removed */",
      "ui_popup_fit drops its off-the-top-of-screen clamp"),
 # NOT LISTED, on purpose: dropping ui_popup_fit's `(row_h > 0) ?` divide guard. It was
 # tried and came back GREEN — arm64's SDIV returns 0 for x/0 rather than trapping, so
 # `vis` is 0 either way and the "at least one row" clamp hides it. The guard is real
 # defence on other hosts; it is simply not observable from a test on this machine, and a
 # mutation that cannot go red does not belong in this list.
 # (3) PDNA_NAV_HEAD and friends: nav_menu places its rows with these now.
 (PL, "#define PDNA_NAV_HEAD    20", "#define PDNA_NAV_HEAD    17",
      "NAV_HEAD 20->17 (rows ride up onto the title divider)"),
 (PL, "#define PDNA_NAV_TITLE_DY 4", "#define PDNA_NAV_TITLE_DY 10", "NAV_TITLE_DY 4->10"),
 (PL, "#define PDNA_NAV_DIV_DY  15", "#define PDNA_NAV_DIV_DY  19", "NAV_DIV_DY 15->19"),
 (PL, "#define PDNA_NAV_PAD      6", "#define PDNA_NAV_PAD      60", "NAV_PAD 6->60"),
 (PL, "#define PDNA_NAV_BAND_DY (-2)", "#define PDNA_NAV_BAND_DY (-4)",
      "NAV_BAND_DY -2->-4 (bar eats the row above)"),
 (PL, "#define PDNA_NAV_BAND_DY (-2)", "#define PDNA_NAV_BAND_DY (0)",
      "NAV_BAND_DY -2->0 (bar eats the row below)"),
 (PL, "#define PDNA_NAV_BAND_H  12", "#define PDNA_NAV_BAND_H  9",
      "NAV_BAND_H 12->9 (bar cuts the label's glyph box)"),
 (PL, "#define PDNA_NAV_ROW_H   11", "#define PDNA_NAV_ROW_H   9", "NAV_ROW_H 11->9"),
 (PL, "#define PDNA_NAV_HINT_DY (-10)", "#define PDNA_NAV_HINT_DY (-30)",
      "NAV_HINT_DY -10->-30 (hint lands on the last row)"),
 # (4) PDNA_FILT_TEXT_X was a decoy: defined, read by neither the screens nor the test.
 (PL, "#define PDNA_FILT_TEXT_X     8", "#define PDNA_FILT_TEXT_X     120",
      "FILT_TEXT_X 8->120 (row text runs off the highlight bar)"),
 (PL, "#define PDNA_FILT_TEXT_X     8", "#define PDNA_FILT_TEXT_X     2",
      "FILT_TEXT_X 8->2 (row text straddles the bar's left edge)"),
 (PL, "#define PDNA_FILT_CHIP_DX   32", "#define PDNA_FILT_CHIP_DX   20",
      "FILT_CHIP_DX 32->20 (type name lands on the colour chip)"),
 (PL, "#define PDNA_FILT_CHIP_W    26", "#define PDNA_FILT_CHIP_W    40",
      "FILT_CHIP_W 26->40 (chip runs under the type name)"),
 (PL, "#define PDNA_FILT_FOOTER_Y 152", "#define PDNA_FILT_FOOTER_Y 148",
      "FILT_FOOTER_Y 152->148 (footer rises above the band popups avoid)"),
 (PL, "#define PDNA_FILT_FOOTER_Y 152", "#define PDNA_FILT_FOOTER_Y 154",
      "FILT_FOOTER_Y 152->154 (footer ink off the bottom of the screen)"),
 (PL, '#define PDNA_FILT_FOOT     "A pick  U/D/L/R move  B back"',
      '#define PDNA_FILT_FOOT     "A pick  U/D/L/R move  B go back"', "filter footer grows"),
 (PL, '#define PDNA_IFILT_FOOT    "A pick  U/D move  B back"',
      '#define PDNA_IFILT_FOOT    "A pick  U/D/L/R move  B go back now"', "item filter footer grows"),
 (PL, '#define PDNA_FILT_SORT_NAME  "A-Z (name)"',
      '#define PDNA_FILT_SORT_NAME  "A-Z (by name, then no.)"', "filter Sort row value grows"),
 (PL, '#define PDNA_FILT_MARKALL    "Mark all..."',
      '#define PDNA_FILT_MARKALL    "Mark every species seen and caught"', '"Mark all..." row grows'),
 (PL, "#define PDNA_SET_FOOTER_Y    152", "#define PDNA_SET_FOOTER_Y    148",
      "SET_FOOTER_Y 152->148 (settings footer rises into the popup band)"),
 (PL, "#define PDNA_SET_FOOTER_Y    152", "#define PDNA_SET_FOOTER_Y    154",
      "SET_FOOTER_Y 152->154 (settings footer ink off the screen)"),
 # --- 2026-08-18: the ROM IMAGE CHECK verdict band --------------------------
 # Six of these strings shipped over the 232 px field at realistic values and clipped to a
 # trailing '~'. Each mutation puts the SHIPPED-AND-CLIPPING wording back, so the entry
 # doubles as the record of what the old line was.
 (PL, '#define PDNA_RVF_COVER_FMT   "%s of %s B (%lu.%01lu%%)%s"',
      '#define PDNA_RVF_COVER_FMT   "%s of %s B checked (%lu.%01lu%%)%s"',
      'coverage header takes "checked" back (rendered as "...(99.4%) b~")'),
 (PL, '#define PDNA_RVF_V_FAULT     "IMAGE FAULT - %d bad, %d unstable, %d skip"',
      '#define PDNA_RVF_V_FAULT     "IMAGE INCOMPLETE - %d bad, %d unstable, %d skip"',
      'fault verdict back to "IMAGE INCOMPLETE" (256 px at 3-digit counts)'),
 (PL, '#define PDNA_RVF_BAD1_FMT    "bad #1: reg %d @ 0x%06lx %lu.%02luMB %s"',
      '#define PDNA_RVF_BAD1_FMT    "bad #1: region %d @ 0x%06lx (%lu.%02lu MB) %s"',
      'bad-region line back to the form that cut off "unstable"'),
 (PL, '#define PDNA_RVF_RETRY_FMT   "%d region(s) needed a retry - the bus lied"',
      '#define PDNA_RVF_RETRY_FMT   "%d region(s) needed a retry - the cart bus lied once"',
      'retry line back to the version that clipped at EVERY count'),
 (PL, '#define PDNA_RVF_V_SHORT     "COVERAGE SHORT - %lu/%lu KiB compared"',
      '#define PDNA_RVF_V_SHORT     "COVERAGE SHORT - %lu of %lu KiB compared"',
      "COVERAGE SHORT back to the spelled-out ratio"),
 (PL, '#define PDNA_RVF_SHORT_NOTE  "Less compared than this image can prove."',
      '#define PDNA_RVF_SHORT_NOTE  "Fewer bytes compared than this image can prove."',
      "short-coverage note back to the 246 px sentence"),
 (PL, '#define PDNA_RVF_V_OK_PART   "IMAGE OK - %d match (%d part), %d skipped"',
      '#define PDNA_RVF_V_OK_PART   "IMAGE OK - %d match (%d partial), %d skipped"',
      '"partial" back in the green verdict (234 px at 3-digit counts)'),
 (PL, '#define PDNA_RVF_LEGEND_ALT  "grn=ok teal=part red=BAD ylw=bus gry=skip"',
      '#define PDNA_RVF_LEGEND_ALT  "grn=ok  teal=part  red=BAD  ylw=bus  gry=skip  X"',
      "the legend FALLBACK grows (nothing measures it at runtime)"),
 (PL, "#define PDNA_RVF_TEXT_W      232", "#define PDNA_RVF_TEXT_W      200",
      "RVF_TEXT_W 232->200 (the band's field narrows under the strings)"),
 (PL, "#define PDNA_RVF_ROW4_Y      152", "#define PDNA_RVF_ROW4_Y      156",
      "RVF_ROW4_Y 152->156 (the blind-range line inks off the bottom)"),
 (PL, "#define PDNA_RVF_ROW1_Y      128", "#define PDNA_RVF_ROW1_Y      124",
      "RVF_ROW1_Y 128->124 (the verdict lands on the grid's bottom rule)"),
 (PL, "#define PDNA_RVF_ROW2_Y      136", "#define PDNA_RVF_ROW2_Y      134",
      "RVF_ROW2_Y 136->134 (row 1's descenders shear into row 2)"),
]

# ---------------------------------------------------------------------------
# DECOUPLING LINT — the other half of the problem, and the half no host test can see.
#
# A shared constant only guards a screen if the SCREEN READS IT. Before 2026-08 these
# same headers were already in place and the test already asserted on them, yet
# pdna_main.c drew its footers at a hard `ui_text(2, 150, ..)` and placed its nav rows at
# `my + 20`, and pdna_pick.c drew its filter rows at `ui_text(8, y, ..)`. Change the
# constant and the test moved; the pixels did not. Both were green the whole time.
#
# So: after the mutations, refuse to pass if a literal has crept back into a wired call
# site. This is a lint, not a fit check — it proves the wiring, the mutations above prove
# the arithmetic.
# Each entry is (path, regex, expected_matches, description). expected_matches == 0 means
# "this literal must not come back"; a positive number is the exact count of wired call
# sites, so deleting the wiring (or the screen) fails just as loudly as re-typing a number.
#
# SCOPE, stated rather than implied: this covers the sites this work wired — the box and
# party footers, "B=back", the two build stamps, nav_menu's rows/columns, and the three
# FILTER / SORT lists. Other screens still spell their own hint row as a literal 152; they
# carry no popups, so nothing about them is asserted here and pretending otherwise would
# be the same false confidence this file exists to remove.
WIRING = [
 ("source/pdna_main.c", r"ui_text\(\s*\d+\s*,\s*150\s*,", 0,
  "a footer drawn at a literal y=150 instead of UI_FOOTER_Y"),
 ("source/pdna_main.c", r"ui_text\([^,]+,\s*UI_FOOTER_Y\s*,", 5,
  "the five footers that must draw AT UI_FOOTER_Y"),
 ("source/pdna_main.c", r"ui_hline\(\s*0\s*,\s*147\s*,", 0,
  "a footer rule at a literal y=147 instead of UI_FOOTER_RULE_Y"),
 ("source/pdna_main.c", r"ui_hline\(\s*0\s*,\s*UI_FOOTER_RULE_Y\s*,", 2,
  "the two footer rules that must derive from UI_FOOTER_Y"),
 ("source/pdna_main.c", r"my \+ 20 \+", 0,
  "a nav row placed at a literal head of 20 instead of PDNA_NAV_HEAD"),
 ("source/pdna_main.c", r"my \+ PDNA_NAV_HEAD \+", 3,
  "the three nav row-placement sites (label, bar lift, bar lay)"),
 ("source/pdna_main.c", r"mx \+ 6 \+", 0,
  "a nav column placed at a literal pad of 6 instead of PDNA_NAV_PAD"),
 ("source/pdna_pick.c", r"ui_text\(\s*8\s*,\s*y\s*,", 0,
  "a filter row drawn at a literal x=8 instead of PDNA_FILT_TEXT_X"),
 ("source/pdna_pick.c", r"ui_text\(\s*40\s*,\s*y\s*,", 0,
  "a type-row name at a literal x=40 instead of TEXT_X + CHIP_DX"),
 ("source/pdna_pick.c", r"PDNA_FILT_TEXT_X", 13,
  "the 13 filter-row draw lines (filter_menu 4, dex_menu 6, item_filter_menu 3)"),
 ("source/pdna_pick.c", r"ui_text\(4, PDNA_FILT_FOOTER_Y,", 3,
  "the three FILTER / SORT footers"),
 # The ROM IMAGE CHECK band. Its strings sat as literals INSIDE pdna_romfull.c until
 # 2026-08-18, where no host test could reach them — which is why six of them shipped
 # clipping. BAND_X / BAND_W are the .c's aliases for the two header constants.
 ("source/pdna_romfull.c", r"ui_ptext_fit\(\s*\d", 0,
  "a band line drawn at a literal x instead of BAND_X"),
 ("source/pdna_romfull.c", r"ui_ptext_fit\(BAND_X,", 12,
  "the 12 band lines, all drawn through the header's geometry"),
 ("source/pdna_romfull.c", r"#define BAND_W\s+PDNA_RVF_TEXT_W", 1,
  "BAND_W must alias the header constant, not re-type 232"),
 ("source/pdna_romfull.c", r'ui_ptext_fit\([^,]+,[^,]+,[^,]+,[^,]+,\s*"', 0,
  "a band string typed at the draw site instead of coming from pdna_layout.h"),
]

CC = ("cc -std=c11 -I source tests/host_textfit_test.c source/ui_font.c "
      "-o /tmp/htf_mut")

def run_test():
    b = subprocess.run(CC, shell=True, capture_output=True, text=True)
    if b.returncode != 0:
        return "BUILD", b.stderr.strip().split("\n")[0]
    r = subprocess.run(["/tmp/htf_mut"], capture_output=True, text=True)
    bit = [l.strip() for l in r.stdout.split("\n") if l.strip().startswith("FAIL")]
    return ("FAIL" if r.returncode != 0 else "green"), (bit[0] if bit else "")

base_state, _ = run_test()
assert base_state == "green", f"baseline is not green: {base_state}"
print("baseline: test is GREEN\n")

bad = []
for path, old, new, label in M:
    orig = open(path).read()
    if orig.count(old) != 1:
        print(f"  ??  {label:<62} anchor not unique ({orig.count(old)}x)")
        bad.append(label); continue
    try:
        open(path, "w").write(orig.replace(old, new))
        state, bit = run_test()
    finally:
        open(path, "w").write(orig)                  # restore, byte for byte
        assert open(path).read() == orig
    ok = state == "FAIL"
    if not ok: bad.append(label)
    print(f"  {'RED ' if ok else '**GREEN**'} {label:<62} {bit[:96]}")

after, _ = run_test()
assert after == "green", "restore failed — test not green again!"
print(f"\nrestored: test is GREEN again")
print(f"{len(M) - len(bad)}/{len(M)} perturbations turned the test red")
if bad:
    print("VACUOUS (did not bite):")
    for b in bad: print("   -", b)

print("\ndecoupling lint — the wired call sites must still read the constants:")
import re
for path, pat, want, why in WIRING:
    src = open(path).read()
    hits = [i + 1 for i, l in enumerate(src.split("\n")) if re.search(pat, l)]
    ok = len(hits) == want
    if not ok: bad.append(f"{path}: {why}")
    print(f"  {'ok  ' if ok else 'FAIL'} {path:22s} {len(hits):2d}/{want}  {why}"
          + ("" if ok else f"  -> line(s) {hits}"))

sys.exit(1 if bad else 0)
