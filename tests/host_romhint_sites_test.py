#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_romhint_sites_test.py -- lane romhint (#437/#438/#439) structural pins (pure text, no build).

the config code's behaviour is proved by host_romhint_cfg_test.py (a real dual-compile round trip); these pins hold the
SHAPE that matters (the welcome's gate, the retired per-session offer, the map-only predicate, the strings). Each pin
is checked on the real source and then against an in-memory MUTANT; a pin green on its mutant is reported as a defect.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
fails = []


def rd(n):
    return (ROOT / "source" / n).read_text(encoding="utf-8")


def body(t, name):
    m = re.search(r"\b" + re.escape(name) + r"\(", t)
    # first definition (a line ending in ') {' after the name)
    for m in re.finditer(r"^[^\n;]*\b" + re.escape(name) + r"\([^;{]*\)\s*\{", t, re.M):
        i = m.end(); d = 1
        while i < len(t) and d:
            d += (t[i] == "{") - (t[i] == "}"); i += 1
        return t[m.start():i]
    return ""


def pin(name, text, pred, mutate):
    if not pred(text):
        fails.append("RED on real source: " + name)
    m = mutate(text)
    if m == text:
        fails.append("mutant is a no-op (pin cannot be proved): " + name)
    elif pred(m):
        fails.append("pin stays GREEN on its mutant: " + name)


mn = rd("pdna_main.c")
lay = rd("pdna_layout.h")

# ---- #438 the welcome's gate: writable card, no Gen-3 ROM, no GB ROM, key absent
pin("#438 welcome gate (writable, no ROM of any kind, key absent)",
    mn,
    lambda t: re.search(r"static void __attribute__\(\(noinline\)\) rom_welcome\(void\) \{\s*"
                        r"if \(!app_can_edit\(\) \|\| app_any_rom_registered\(\) \|\|\s*"
                        r"app_gb_rom_path\(PDNA_GEN1\)\[0\] \|\| app_gb_rom_path\(PDNA_GEN2\)\[0\]\) return;\s*"
                        r"if \(rom_welcome_asked\(\)\) return;", t) is not None,
    lambda t: t.replace("if (rom_welcome_asked()) return;", "", 1))
pin("#438 welcome writes romask=1 for EITHER answer, before the menu opens",
    mn,
    lambda t: re.search(r'wait_keys\(KEY_A \| KEY_B\);\s*cfg_save_ex\("romask", "1", NULL\);\s*if \(k & KEY_A\)\s*\{ snd_ok\(\); ui_clear\(\); rom_row_menu\(\); \}', body(t, "rom_welcome")) is not None,
    lambda t: t.replace('cfg_save_ex("romask", "1", NULL);', 'if (k & KEY_B) cfg_save_ex("romask", "1", NULL);', 1))
pin("#438 main shows it after cfg_load and before the first save picker",
    mn,
    lambda t: (lambda mb: mb.find("cfg_load();") >= 0 and mb.find("cfg_load();") < mb.find("rom_welcome();") < mb.find("browse_pick(path, sizeof(path))"))(t[t.index("int main(void) {"):]),
    lambda t: t.replace("  rom_welcome();                             /* #438: once per card, before the first save picker */\n", "", 1))
# ---- #438 the key: absent until answered, round-tripped by every other cfg_save, parsed as a no-op
pin("#438 cfg_save_ex writes romask=1 only when set (old files stay byte-identical until then)",
    mn,
    lambda t: re.search(r'if \(romask && !truncated\) \{\s*int w = sniprintf\(buf \+ n, sizeof\(buf\) - \(size_t\)n, "romask=1\\n"\);', t) is not None,
    lambda t: t.replace("if (romask && !truncated) {", "if (!truncated) {", 1))
pin("#438 cfg_save_ex round-trips the old key (an unrelated save never erases the answer)",
    mn,
    lambda t: re.search(r'find_key_in_text\(buf, br, "romask", ask, \(int\)sizeof ask\) && ask\[0\] == \'1\'', t) is not None
              and "cfg_read_old_dirkeys(old_dirrom, old_dirgb, old_dirgbsav, &romask);" in t,
    lambda t: t.replace("*romask = find_key_in_text(buf, br, \"romask\", ask, (int)sizeof ask) && ask[0] == '1';", "*romask = false;", 1)
              if "*romask = find_key_in_text" in t else t.replace('"romask", ask', '"romaskX", ask', 1))
pin("#438 the active_key path sets it from the caller's value",
    mn,
    lambda t: 'else if (!strcmp(active_key, "romask"))    romask = (active_val[0] == \'1\');' in t,
    lambda t: t.replace('romask = (active_val[0] == \'1\');', 'romask = false;', 1))
# ---- #438 the old per-session offer is retired for anyone who answered
pin("#438/fix2 #1 the per-session save-open offer fires only for a user with NO ROM registered who never answered",
    mn,
    lambda t: re.search(r'if \(!offered && !boxoam_icons_available\(\) && app_can_edit\(\)\) \{\s*offered = true;\s*'
                        r'(?:/\*.*?\*/\s*)?if \(!app_any_rom_registered\(\) && !rom_welcome_asked\(\)\) \{', t, re.S) is not None,
    lambda t: t.replace("if (!app_any_rom_registered() && !rom_welcome_asked()) {", "if (!rom_welcome_asked()) {", 1))
pin("fix2 #7 cfg_load parses rstr/rdur and applies them (the config round trip keeps the rumble strength/duration)",
    mn,
    lambda t: re.search(r'!strcmp\(k, "rstr"\)\)\s*\{[^}]*rmbl_set_strength\(m\);\s*\}', t) is not None
              and re.search(r'!strcmp\(k, "rdur"\)\)\s*\{[^}]*rmbl_set_duration\(m\);\s*\}', t) is not None,
    lambda t: t.replace('rmbl_set_duration(m);', '(void)m;', 1))
pin("fix2 #8 the welcome draws its lines from the X-macro count (no hard-coded 5 / i == 4)",
    mn,
    lambda t: "PDNA_ROMWEL_LINES(ROMWEL_ELT)" in body(t, "rom_welcome") and "k_line[5]" not in t and "i == 4 ? UI_DIM" not in t,
    lambda t: t.replace("k_line[] = {", "k_line[5] = {", 1))
pin("fix3 D1 summary: no sprite -> anim=false right after the fetch (else portrait_redraw erases the no-art text)",
    rd("pdna_summary.c"),
    lambda t: re.search(r'if \(anim && !p_spr_ok\) \{ p_spr = portrait_sprite\([^\n]*\n\s*if \(!p_spr\) anim = false;', t) is not None,
    lambda t: t.replace("    if (!p_spr) anim = false;", "    /* mutant */", 1))
pin("fix2 #4 the summary ability row draws the placeholder as two explicit fitted lines",
    rd("pdna_summary.c"),
    lambda t: re.search(r'if \(app_desc_is_placeholder\(ad\)\) \{[^}]*ui_ptext_fit\(x \+ 4, y, INFO_W - 4, UI_DIM, PDNA_DESC_PLACEHOLDER_L1\); y \+= UI_ROW_H;\s*'
                        r'ui_ptext_fit\(x \+ 4, y, INFO_W - 4, UI_DIM, PDNA_DESC_PLACEHOLDER_L2\); y \+= UI_ROW_H;', t) is not None,
    lambda t: t.replace("PDNA_DESC_PLACEHOLDER_L2); y += UI_ROW_H;", "PDNA_DESC_PLACEHOLDER_L2); y += 0;", 1))
pin("fix2 #3 the Game Boy create dialog and the yard refusal name PDNA_ROM_WHERE (one term)",
    lay,
    lambda t: '#define PDNA_GBCREATE_NOROM_L2       "(" PDNA_ROM_WHERE ")."' in t and '"(" PDNA_ROM_WHERE ").");' in mn,
    lambda t: t.replace('"(" PDNA_ROM_WHERE ")."', '"(Settings > Game ROM)."', 1))
pin("fix2 #6 the item picker's line buffer is bounded (sniprintf into char d[96])",
    rd("pdna_pick.c"),
    lambda t: 'sniprintf(d, sizeof d, "%s", idesc)' in t and 'sniprintf(d, sizeof d, "%s  %s", nm, idesc)' in t,
    lambda t: t.replace('sniprintf(d, sizeof d, "%s  %s", nm, idesc)', 'siprintf(d, "%s  %s", nm, idesc)', 1))
# ---- #439 map-only: the predicate is a registered PATH, never s_iconrom.ok
pin("#439 the Game ROM row says 'map only' for a registered path with no icon tables",
    mn,
    lambda t: re.search(r"s_iconrom\.ok \? rom_kind_name\(s_iconrom_ctx\.kind\)\s*:\s*app_any_rom_registered\(\) \? PDNA_SET_ROM_MAPONLY", t) is not None,
    lambda t: t.replace(": app_any_rom_registered() ? PDNA_SET_ROM_MAPONLY", ": 0 ? PDNA_SET_ROM_MAPONLY", 1))
pin("#439 the Extract art row says 'ROM has no art' for the same state",
    mn,
    lambda t: re.search(r"art_off_reg \? PDNA_SET_ART_ARTOFF\s*:\s*app_any_rom_registered\(\) \? PDNA_SET_ART_MAPONLY", t) is not None,
    lambda t: t.replace(": app_any_rom_registered() ? PDNA_SET_ART_MAPONLY", ": 0 ? PDNA_SET_ART_MAPONLY", 1))
# ---- #437 one term; the art-off / unreadable reasons stay pointer-free
pin("#437 every no-ROM pointer is built from PDNA_ROM_WHERE",
    lay,
    lambda t: re.search(r'#define PDNA_ROM_WHERE_A "Settings >"', t) is not None
              and re.search(r'#define PDNA_ROM_WHERE_B "Game ROM"', t) is not None
              and re.search(r'#define PDNA_ROM_WHERE PDNA_ROM_WHERE_A " " PDNA_ROM_WHERE_B', t) is not None
              and t.count("PDNA_ROM_WHERE") >= 5
              and re.search(r'PDNA_GBSCR_REASON_NO_ROM\s+"Add a ROM: " PDNA_ROM_WHERE', t) is not None
              and re.search(r'PDNA_DESC_PLACEHOLDER_L1 "\(Add a Gen-3 ROM:"', t) is not None
              and re.search(r'PDNA_DESC_PLACEHOLDER_L2 PDNA_ROM_WHERE "\)"', t) is not None,
    lambda t: t.replace('"Add a ROM: " PDNA_ROM_WHERE   /* #437/romhint F4 */', '"no ROM - Settings"   /* #437/romhint F4 */', 1))
# ---- romhint F2: the placeholder splits by WHY there is no text
pin("romhint F2 art-off / no-text placeholders never say 'add a ROM'",
    lay,
    lambda t: re.search(r'PDNA_DESC_ROMOFF\s+"\(" PDNA_SET_ART_ARTOFF "\)"', t) is not None
              and re.search(r'PDNA_DESC_NOTEXT\s+"\(no text in this ROM\)"', t) is not None,
    lambda t: t.replace('"(" PDNA_SET_ART_ARTOFF ")"', '"(Add a ROM: " PDNA_ROM_WHERE ")"', 1))
pin("romhint F2 desc_or_fallback swaps the placeholder only when a ROM IS registered (off -> ROMOFF, else NOTEXT)",
    mn,
    lambda t: re.search(r'if \(strcmp\(embedded, PDNA_DESC_PLACEHOLDER\) == 0 && app_any_rom_registered\(\)\)\s*'
                        r'return g_rom_art_off \? PDNA_DESC_ROMOFF : PDNA_DESC_NOTEXT;', body(t, "desc_or_fallback")) is not None,
    lambda t: t.replace("strcmp(embedded, PDNA_DESC_PLACEHOLDER) == 0 && app_any_rom_registered()", "strcmp(embedded, PDNA_DESC_PLACEHOLDER) == 0", 1))
pin("romhint F2 the item picker's one-line view shows the long placeholder alone",
    rd("pdna_pick.c"),
    lambda t: "if (g_item_max_id || app_desc_is_placeholder(idesc)) sniprintf(d, sizeof d, \"%s\", idesc);" in t,
    lambda t: t.replace("g_item_max_id || app_desc_is_placeholder(idesc)", "g_item_max_id", 1))
# ---- romhint F3: nothing registered -> "GBA ROM: not set", no art toggle
pin("romhint F3 rom_row_menu: unregistered shows 'GBA ROM: not set' and hides the art toggle",
    mn,
    lambda t: re.search(r'const bool g3_reg = app_any_rom_registered\(\);\s*'
                        r'rows\[nr\] = g3_reg \? "Change ROM" : "GBA ROM: not set";\s*act\[nr\+\+\] = 0;\s*'
                        r'if \(g3_reg\) \{ rows\[nr\] = g_rom_art_off \? "Turn ROM art ON" : "Turn ROM art OFF"; act\[nr\+\+\] = 1; \}', t) is not None,
    lambda t: t.replace("if (g3_reg) { rows[nr] = g_rom_art_off", "if (1) { rows[nr] = g_rom_art_off", 1))
pin("#437 art-off and unreadable-ROM texts never tell the user to add a ROM",
    lay,
    lambda t: re.search(r'PDNA_SET_ART_ARTOFF\s+"ROM art is off"', t) is not None
              and re.search(r'PDNA_GBSCR_REASON_OPEN\s+"ROM art unavailable"', t) is not None,
    lambda t: t.replace('"ROM art unavailable"', '"add a ROM: " PDNA_ROM_WHERE', 1))

# ---- romhint2 #448: the three-case pointer (Gen-3 ROM / GB-only / nothing) and its three call sites
summ = rd("pdna_summary.c")
pin("romhint2 #448b the pointer has THREE cases: Gen-3 ROM -> plain 'no art'; GB-only -> Gen-3 wording; nothing -> 'Add a ROM:'",
    body(summ, "pdna_summary_noart_pointer_in"),
    lambda t: (lambda i1, i2, i3: 0 <= t.find("app_any_rom_registered()") < t.find("PDNA_DEX_NOART_G3_L2") < t.find("PDNA_DEX_NOART_L2")
               and "app_gb_rom_path(PDNA_GEN1)[0]" in t[i1:i2] and "app_gb_rom_path(PDNA_GEN2)[0]" in t[i1:i2])(
        t.find("app_any_rom_registered()"), t.find("else {"), 0),
    lambda t: t.replace("PDNA_DEX_NOART_G3_L2", "PDNA_DEX_NOART_L1", 1))
pin("romhint2 #448 all three call sites (summary, dex detail, box pane) reach the same helper family",
    summ + rd("pdna_pick.c") + rd("pdna_box.c"),
    lambda t: t.count("pdna_summary_noart_pointer()") >= 2 and "pdna_summary_noart_pointer_in(PDNA_BOX_NOART_FX, PDNA_BOX_NOART_FW, PDNA_BOX_NOART_DY)" in t,
    lambda t: t.replace("pdna_summary_noart_pointer_in(PDNA_BOX_NOART_FX", "(void)(PDNA_BOX_NOART_FX", 1))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_romhint_sites_test: all pins green on source and red on mutants")
