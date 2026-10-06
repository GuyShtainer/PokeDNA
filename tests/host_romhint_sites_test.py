#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_romhint_sites_test.py -- lane romhint (#437/#438/#439) structural pins (pure text, no build).

config.cfg parsing/writing lives in pdna_main.c next to FatFs, so it cannot dual-compile on the host; these pins hold the
shape that matters (the welcome's gate, the key's round trip, the retired per-session offer, the map-only predicate). Each pin
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
    lambda t: re.search(r'wait_keys\(KEY_A \| KEY_B\);\s*cfg_save_ex\("romask", "1", NULL\);\s*if \(k & KEY_A\)\s*\{ snd_ok\(\); rom_row_menu\(\); \}', body(t, "rom_welcome")) is not None,
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
pin("#438 the per-session save-open offer is gated on the key being absent",
    mn,
    lambda t: "if (!offered && !boxoam_icons_available() && app_can_edit() && !rom_welcome_asked()) {" in t,
    lambda t: t.replace(" && app_can_edit() && !rom_welcome_asked()) {", " && app_can_edit()) {", 1))
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
              and re.search(r'PDNA_DESC_PLACEHOLDER "\(Add a ROM: " PDNA_ROM_WHERE "\)"', t) is not None,
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
    lambda t: "if (g_item_max_id || app_desc_is_placeholder(idesc)) siprintf(d, \"%s\", idesc);" in t,
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

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_romhint_sites_test: all pins green on source and red on mutants")
