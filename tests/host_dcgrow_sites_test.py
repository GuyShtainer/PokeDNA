#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_dcgrow_sites_test.py -- source pins for BACKLOG #373 (lane zi): the Day-Care take-out growth flow in BOTH twins
(Gen 3 dc_withdraw in pdna_main.c, Gen 1/2 gbdc_take in pdna_gbdaycare.c) and the shared GREW IN DAY-CARE panel.
Each pin carries a mutant that must turn it red.

    python3 tests/host_dcgrow_sites_test.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = (ROOT / "source" / "pdna_main.c").read_text()
GBDC = (ROOT / "source" / "pdna_gbdaycare.c").read_text()
GEN12 = (ROOT / "source" / "pdna_gen12.c").read_text()


def body(text, name):
    m = re.search(r"\n[^\n]*\b" + re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not m:
        return ""
    i = text.index("{", m.start())
    depth = 0
    for j in range(i, len(text)):
        depth += text[j] == "{"
        depth -= text[j] == "}"
        if depth == 0:
            return text[i:j + 1]
    return ""


def before(s, a, b):
    ia, ib = s.find(a), s.find(b)
    return ia >= 0 and ib >= 0 and ia < ib


def checks(main, gbdc, gen12):
    out = []
    w = body(main, "dc_withdraw")
    ask = body(main, "dc_grow_ask")
    # P1: the Gen-3 flow asks FIRST: after the read-only guard, before the destination menu
    if not before(w, "app_can_edit()", "dc_grow_ask("):
        out.append("P1: dc_withdraw must keep the read-only guard BEFORE dc_grow_ask")
    if not before(w, "dc_grow_ask(", "static const char* const D[3]"):
        out.append("P1: dc_withdraw must run dc_grow_ask BEFORE the To Party / To PC destination menu")
    if not re.search(r"if \(!dc_grow_ask\([^)]*\)\) return false;", w):
        out.append("P1: a 'Leave inside' (dc_grow_ask false) must return false from dc_withdraw with nothing written")
    # P2: preview is the pure core, no panel when the level did not move, LEAVE writes nothing
    if "gen3_dc_preview(g_game, rec, false, dc_steps_of(base, stride, physi), g)" not in ask:
        out.append("P2: dc_grow_ask must preview with the SAVE'S game and the slot's own steps")
    if "if (g->lv_after == g->lv_before) return true;" not in ask:
        out.append("P2: no panel (today's flow) when the level did not change")
    if not re.search(r"if \(pick == PDNA_DCG_LEAVE\) return false;", ask):
        out.append("P2: Leave inside must return false")
    # P3: the steps address == the one dc_clear_slot_aux zeroes
    st = body(main, "dc_steps_of")
    if "base + 272 + (uint32_t)physi * 4" not in st or "base + (uint32_t)physi * stride + 136" not in st:
        out.append("P3: dc_steps_of must read RS base+272+i*4 / E,FRLG base+i*stride+136")
    # P4: apply BEFORE em_set_party_flag (To Party) and before the PC inject (To PC)
    if not before(w, "gen3_dc_apply(&e, grow, with_moves);                             /* grown BEFORE", "em_set_party_flag(&e, true);"):
        out.append("P4: To Party must gen3_dc_apply BEFORE em_set_party_flag")
    if not before(w, "gen3_dc_apply(&e, grow, with_moves);\n      gen3_edit_commit(&e, grown);", "app_inject_to_game_deferred(src, &pb, &ps)"):
        out.append("P4: To PC must build the grown 80-byte copy (apply + gen3_edit_commit) before the inject")
    # P5: the panel strings (shared panel lives in pdna_gbdaycare.c)
    pn = body(gbdc, "pdna_dc_grow_panel")
    for s in ('"GREW IN DAY-CARE"', '"Leave inside"', '"Take, learn moves"', '"Take, keep moves"', '"Take out"',
              '"No new moves."', '"Moves: needs the game ROM"', '"...and %d more"', '"Moves now:"',
              '"Lv %d -> Lv %d  (+%d)"', '"%s replaces %s"', '"%s learned"'):
        if s not in pn + body(gbdc, "dcg_event_line"):
            out.append("P5: panel string missing: " + s)
    if "if (no_rom)               { opts[nopt] = \"Take, keep moves\"; act[nopt++] = PDNA_DCG_KEEP; }" not in pn:
        out.append("P5: the no-ROM panel must offer ONLY Leave inside / Take, keep moves")
    if "if (k & KEY_B) return PDNA_DCG_LEAVE;" not in pn:
        out.append("P5: B must be Leave inside")
    # P6: Gen 1/2 twin: preview BEFORE gbd_withdraw; LEAVE returns before it; apply after it, before gbdc_land
    t = body(gbdc, "gbdc_take")
    if not before(t, "gbdc_grow_preview(", "gbd_withdraw("):
        out.append("P6: gbdc_take must preview BEFORE gbd_withdraw commits anything")
    if not before(t, "if (pick == PDNA_DCG_LEAVE) return;", "gbd_withdraw("):
        out.append("P6: gbdc_take must return on Leave inside BEFORE gbd_withdraw")
    if not before(t, "gbdc_grow_apply(&mon", "gbdc_land(s, &mon"):
        out.append("P6: gbdc_take must apply the growth to the withdrawn mon BEFORE gbdc_land")
    if not before(t, "gbd_withdraw(s, slot, &mon)", "gbdc_grow_apply(&mon"):
        out.append("P6: gbdc_take must apply the growth AFTER gbd_withdraw filled the mon")
    # P7: no ROM never silently teaches nothing: the no-ROM flag comes from gb_daycare_learn < 0
    pv = body(gbdc, "gbdc_grow_preview")
    if "*no_rom = kept < 0;" not in pv or "gb_daycare_learn(" not in pv:
        out.append("P7: gbdc_grow_preview must set no_rom from gb_daycare_learn < 0")
    if "pdna_dc_grow_panel(grow, grow_norom)" not in t:
        out.append("P7: gbdc_take must hand the no-ROM flag to the panel")
    # P8: Gen 1 keeps the grown EXP, Gen 2 floors it to the level
    ap = body(gbdc, "gbdc_grow_apply")
    if "if (gen == GB_GEN1) (void)gb_set_exp(mon, gb_get_exp(mon));" not in ap or "gb_set_level(mon, g->lv_after)" not in ap:
        out.append("P8: Gen 1 must keep EXP (gb_set_exp), Gen 2 must floor it (gb_set_level)")
    # P9: one log line per take, both twins
    if 'log_line("daycare: take %s Lv%d->%d +%d moves(%s)"' not in ask:
        out.append("P9: the Gen-3 take must log 'daycare: take ...'")
    if 'log_line("daycare: take %s Lv%d->%d +%d moves(%s)"' not in t:
        out.append("P9: the Gen-1/2 take must log 'daycare: take ...'")
    # P10: gb_daycare_learn shares the open/cache helper with gb_create_learn (both PDNA_DELTA arms live in the helper)
    dl = body(gen12, "gb_daycare_learn")
    if "gb_create_locate_rom(gen)" not in dl or "gb_learn_begin()" not in dl or "gb_learn_end();" not in dl:
        out.append("P10: gb_daycare_learn must locate the ROM and open via gb_learn_begin/gb_learn_end")
    if "gb_learn_begin()" not in body(gen12, "gb_create_learn"):
        out.append("P10: gb_create_learn must open through the shared gb_learn_begin")
    return out


def main():
    v = checks(MAIN, GBDC, GEN12)
    if v:
        for x in v:
            print("FAIL:", x)
        return 1
    print("ok: #373 dc_withdraw/gbdc_take ask first, panel strings, apply-before-party-flag, no-ROM branch, EXP rules, log lines, shared learn open")
    fails = 0
    muts = (
        ("P1 menu before ask", "main", "if (!dc_grow_ask(base, stride, rec, physi, &grow, &with_moves)) return false;   /* Leave inside */", ""),
        ("P1 leave ignored", "main", "if (!dc_grow_ask(base, stride, rec, physi, &grow, &with_moves)) return false;", "(void)dc_grow_ask(base, stride, rec, physi, &grow, &with_moves);"),
        ("P2 panel on every take", "main", "if (g->lv_after == g->lv_before) return true;", ""),
        ("P2 leave not false", "main", "if (pick == PDNA_DCG_LEAVE) return false;", "if (pick == PDNA_DCG_LEAVE) return true;"),
        ("P2 wrong game", "main", "gen3_dc_preview(g_game, rec", "gen3_dc_preview(PK_EMERALD, rec"),
        ("P3 steps offset", "main", "base + 272 + (uint32_t)physi * 4", "base + 270 + (uint32_t)physi * 4"),
        ("P4 apply after party flag", "main", "    if (grow) gen3_dc_apply(&e, grow, with_moves);                             /* grown BEFORE the party form */\n    em_set_party_flag(&e, true);", "    em_set_party_flag(&e, true);\n    if (grow) gen3_dc_apply(&e, grow, with_moves);"),
        ("P4 PC copy dropped", "main", "      gen3_dc_apply(&e, grow, with_moves);\n      gen3_edit_commit(&e, grown);\n      src = grown;", "      src = rec;"),
        ("P5 title", "gbdc", 'UI_TITLE, "GREW IN DAY-CARE")', 'UI_TITLE, "GREW UP")'),
        ("P5 option renamed", "gbdc", '"Take, learn moves"', '"Take"'),
        ("P5 no-ROM offers learn", "gbdc", 'if (no_rom)               { opts[nopt] = "Take, keep moves"; act[nopt++] = PDNA_DCG_KEEP; }', 'if (0)               { opts[nopt] = "Take, keep moves"; act[nopt++] = PDNA_DCG_KEEP; }'),
        ("P5 B takes", "gbdc", "if (k & KEY_B) return PDNA_DCG_LEAVE;", "if (k & KEY_B) return PDNA_DCG_KEEP;"),
        ("P6 withdraw first", "gbdc", "  DcGrow* grow = pdna_dc_grow_buf();\n  bool grew = false, grow_norom = false, with_moves = false;\n  if (slot >= 0 && slot < 2 && dc->slot[slot].occupied) {", "  DcGrow* grow = pdna_dc_grow_buf();\n  bool grew = false, grow_norom = false, with_moves = false;\n  GbEditMon mon0; (void)gbd_withdraw(s, slot, &mon0);\n  if (slot >= 0 && slot < 2 && dc->slot[slot].occupied) {"),
        ("P6 leave falls through", "gbdc", "if (pick == PDNA_DCG_LEAVE) return;                 /* nothing written */", ""),
        ("P6 apply after land", "gbdc", "    gbdc_grow_apply(&mon, s->gen, grow, with_moves);\n    log_line", "    log_line"),
        ("P7 no_rom lost", "gbdc", "*no_rom = kept < 0;", "*no_rom = false;"),
        ("P8 gen1 floors", "gbdc", "if (gen == GB_GEN1) (void)gb_set_exp(mon, gb_get_exp(mon));", "if (gen == GB_GEN1) (void)gb_set_level(mon, g->lv_after);"),
        ("P9 gen3 log dropped", "main", 'log_line("daycare: take %s Lv%d->%d +%d moves(%s)"', 'log_line("daycare: grow %s Lv%d->%d +%d moves(%s)"'),
        ("P9 gb log dropped", "gbdc", 'log_line("daycare: take %s Lv%d->%d +%d moves(%s)"', 'log_line("daycare: grow %s Lv%d->%d +%d moves(%s)"'),
        ("P10 private open", "gen12", "  if (!gb_learn_begin()) return -1;\n  int kept = rom_gblearn_moves_between", "  int kept = rom_gblearn_moves_between"),
    )
    for label, which, a, b in muts:
        srcs = {"main": MAIN, "gbdc": GBDC, "gen12": GEN12}
        if a not in srcs[which]:
            print(f"FAIL: mutation {label} target not found"); fails += 1; continue
        srcs[which] = srcs[which].replace(a, b, 1)
        if checks(srcs["main"], srcs["gbdc"], srcs["gen12"]):
            print(f"mutation {label}: correctly caught")
        else:
            print(f"FAIL: mutation {label} did NOT turn a check red"); fails += 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
