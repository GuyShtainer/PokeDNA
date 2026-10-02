#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_zy_sites_test.py -- structural pins for lane zy (BACKLOG #378/#379/#380/#381) on the REAL sources that
cannot link on the host (pdna_pick.c, pdna_bank.c, pdna_gen12.c are tonc-bound).

  (a) #380 pdna_pick.c: the bulk op snapshots the Emerald sort-order byte (dex_getord_call into
      s_dex_snap[DEX_SNAP_ORD]) BEFORE it writes the dex (the `dex_dset(nat, a)` loop), and the Undo branch
      restores it (dex_setord_call(s_dex_snap[DEX_SNAP_ORD])) AFTER dex_setnat_call(s_dex_snap_natl) -- the
      lock/unlock transition is what zeroes the byte -- and before s_dex_snap_valid is cleared.
  (b) #380 pdna_pick.c: setord is gated by can_edit exactly like setnat; pdna_main.c hands the screen
      dex_get_order / dex_set_order; the GB callers pass NULL, NULL.
  (c) #378 pdna_bank.c box_load classifies through bml_box_read, heals through bml_box_heal only when
      app_can_edit(), logs the restore line, and returns false when the heal failed.
  (d) #379 pdna_bank.c meta_load handles BML_READ_ERROR without meta_defaults() after a valid load and
      meta_save() refuses while g_meta_state == 2.
  (e) mutation self-tests: each pin goes RED on an in-memory mutant of the real text.
"""
import re, sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "source"


def body(text, sig):
    i = text.index(sig)
    j = text.index("{", i)
    d = 0
    for k in range(j, len(text)):
        d += text[k] == "{"
        d -= text[k] == "}"
        if d == 0:
            return text[j:k + 1]
    raise ValueError(sig)


def check_380(pick, main, gbdex):
    f = []
    scr = body(pick, "static bool dex_bulk(") if "static bool dex_bulk(" in pick else pick
    snap = scr.find("s_dex_snap[DEX_SNAP_ORD] = (int8_t)dex_getord_call()")
    setall = scr.find("dex_dset(nat, a)")
    if snap < 0: f.append("bulk op does not snapshot the sort-order byte")
    elif setall < 0 or snap > setall: f.append("sort-order snapshot is not BEFORE the dex_dset(nat, a) write loop")
    undo = scr.find("dex_setord_call((uint8_t)s_dex_snap[DEX_SNAP_ORD])")
    natl = scr.find("dex_setnat_call(s_dex_snap_natl)")
    valid = scr.find("s_dex_snap_valid = false;", natl if natl >= 0 else 0)
    if undo < 0: f.append("Undo does not restore the sort-order byte")
    elif not (0 <= natl < undo < valid): f.append("Undo restore is not AFTER dex_setnat_call(s_dex_snap_natl) and before the valid flag clears")
    if "s_dex_ord.setord = (ord && can_edit) ? ord->setord : NULL" not in pick: f.append("setord is not can_edit-gated")
    if not re.search(r"pdna_dex_screen\(dex_state, dex_set_state, dex_get_national, dex_set_national,\s*&k_dex_ord, app_can_edit\(\)\)", main):
        f.append("pdna_main.c does not pass &k_dex_ord")
    if "k_dex_ord = { dex_get_order, dex_set_order }" not in main: f.append("k_dex_ord does not wire dex_get_order/dex_set_order")
    if gbdex.count("gbdex_shim_set, NULL, NULL, NULL, can_edit)") != 2: f.append("GB callers do not pass NULL ord hooks")
    return f


def check_bank(bank):
    f = []
    bl = body(bank, "static bool box_load(")
    if "bml_box_read(path, box_recs(), BOX_BYTES, &sz)" not in bl: f.append("box_load does not classify through bml_box_read")
    if not re.search(r"if \(app_can_edit\(\)\) \{[^\n]*\n\s*rmbl_pause\(\);\s*healed = bml_box_heal\(path, bsrc, box_recs\(\), BOX_BYTES\);", bl): f.append("box heal is not gated on app_can_edit()")
    if "primary missing, restored from" not in bl: f.append("restore log line missing")
    if "got && !heal_failed" not in bl: f.append("box_load does not return false after a failed heal")
    ml = body(bank, "static bool __attribute__((noinline)) meta_load(")
    m = re.search(r"if \(src == BML_READ_ERROR\) \{(.*?)return false;\s*\}", ml, re.S)
    if not m: f.append("meta_load has no BML_READ_ERROR branch")
    else:
        code = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
        calls = [x.start() for x in re.finditer(r"meta_defaults\(\);", code)]
        if len(calls) != 1 or "if (g_meta_state == 0) { meta_defaults(); g_meta_state = 2; }" not in code:
            f.append("meta_load READ_ERROR may call meta_defaults() after a valid load")
    ms = body(bank, "static bool meta_save(")
    if not re.match(r"\{\s*if \(g_meta_state == 2\)", ms): f.append("meta_save does not refuse at g_meta_state == 2 first thing")
    return f


def main():
    pick = (SRC / "pdna_pick.c").read_text(); mn = (SRC / "pdna_main.c").read_text()
    gb = (SRC / "pdna_gbdex.c").read_text(); bank = (SRC / "pdna_bank.c").read_text()
    fails = check_380(pick, mn, gb) + check_bank(bank)
    # (e) mutants: every one must turn the matching check RED
    muts = [
        ("380 no snapshot", lambda p, m, g, b: (p.replace("s_dex_snap[DEX_SNAP_ORD] = (int8_t)dex_getord_call();", ""), m, g, b)),
        ("380 no restore", lambda p, m, g, b: (p.replace("dex_setord_call((uint8_t)s_dex_snap[DEX_SNAP_ORD]);", ""), m, g, b)),
        ("380 restore before setnat", lambda p, m, g, b: (p.replace("dex_setnat_call(s_dex_snap_natl);", "(void)0;").replace("for (int nat = 1; nat <= s_dex_max; nat++) dex_dset(nat, s_dex_snap[nat - 1]);", "dex_setord_call((uint8_t)s_dex_snap[DEX_SNAP_ORD]); dex_setnat_call(s_dex_snap_natl);").replace("        dex_setord_call((uint8_t)s_dex_snap[DEX_SNAP_ORD]);\n", ""), m, g, b)),
        ("380 setord ungated", lambda p, m, g, b: (p.replace("(ord && can_edit) ? ord->setord", "ord ? ord->setord"), m, g, b)),
        ("380 main not wired", lambda p, m, g, b: (p, m.replace("&k_dex_ord, app_can_edit()", "NULL, app_can_edit()"), g, b)),
        ("378 heal ungated", lambda p, m, g, b: (p, m, g, b.replace("if (app_can_edit()) {                       /* Zy D7", "if (1) {                       /* Zy D7"))),
        ("378 heal fail ignored", lambda p, m, g, b: (p, m, g, b.replace("got && !heal_failed", "got"))),
        ("378 not classified", lambda p, m, g, b: (p, m, g, b.replace("bml_box_read(path", "sf_read_full(path"))),
        ("379 save unguarded", lambda p, m, g, b: (p, m, g, b.replace("if (g_meta_state == 2) {   /* BACKLOG #379", "if (0) {   /* BACKLOG #379"))),
        ("379 defaults after valid", lambda p, m, g, b: (p, m, g, b.replace("if (g_meta_state == 0) { meta_defaults(); g_meta_state = 2; }", "{ meta_defaults(); g_meta_state = 2; }"))),
    ]
    for name, fn in muts:
        p, m, g, b = fn(pick, mn, gb, bank)
        if (p, m, g, b) == (pick, mn, gb, bank):
            fails.append(f"mutant '{name}' did not change the text (test is vacuous)")
        elif not (check_380(p, m, g) + check_bank(b)):
            fails.append(f"mutant '{name}' was NOT caught")
    if fails:
        print("FAIL host_zy_sites_test.py:"); [print("  -", x) for x in fails]; return 1
    print(f"PASS host_zy_sites_test.py ({len(muts)} mutants seen RED)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
