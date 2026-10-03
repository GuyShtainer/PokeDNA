#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_za2_sites_test.py -- structural pins for lane za2 (BACKLOG #389 #390 #387 #388 #391 #386).
Pure text checks against the shipped source (no build). Every fact function is shared by the real check and
by in-memory MUTATIONS that must turn it red (self-test in run()).

  #389  the ledger heal (xfer_heal.c) is CALLED before anything reads a ledger: at both boots, at a Gen-3 save
        load (before the migration), and at the TRANSFERS scan; always with app_can_edit() as the write gate.
  #390  a Gen-2 destination whose fill came up empty for want of a ROM says NO GEN-2 ROM (both zero-move sites).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip_comments(text: str) -> str:
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i:i + 2] == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text[i:i + 2] == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def function_body(text: str, name: str) -> str:
    text = strip_comments(text)
    m = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.MULTILINE)
    if not m:
        return ""
    depth, j = 1, m.end()
    while j < len(text) and depth > 0:
        depth += (text[j] == "{") - (text[j] == "}")
        j += 1
    return text[m.start():j]


def mutate(body: str, old: str, new: str) -> str:
    assert old in body, f"mutation anchor not found: {old!r}"
    return body.replace(old, new, 1)


def run_muts(label_prefix: str, fact, parts: dict, muts) -> None:
    """Each mutation edits one named part; the fact over the edited parts must turn red."""
    for label, which, old, new in muts:
        if old not in parts[which]:
            check(False, f"mutation anchor missing ({label_prefix}: {label})")
            continue
        p2 = dict(parts)
        p2[which] = mutate(parts[which], old, new)
        ok, _ = fact(**p2)
        check(not ok, f"MUT {label_prefix}: {label} was NOT caught")


# ------------------------------------------------------------------------------------------------ #389
def f389(heal: str, boot: str, load: str, walk: str, main: str, dmain: str) -> tuple[bool, str]:
    if not re.search(r"xh_heal_dir\(PDNA_XFER_DIR,\s*scratch,\s*GBSC_FILE_MAX,\s*app_can_edit\(\),", heal):
        return False, "app_ledger_heal: xfer dir not healed with app_can_edit() as the write gate (#389)"
    if not re.search(r"xh_heal_dir\(PDNA_SIDECAR_DIR,\s*scratch,\s*GBSC_FILE_MAX,\s*app_can_edit\(\),", heal):
        return False, "app_ledger_heal: legacy sidecar dir not healed (#389)"
    if not re.search(r"rmbl_pause\(\);.*xh_heal_dir.*rmbl_resume\(\);", heal, re.S):
        return False, "app_ledger_heal: rumble not paused around the renames (#389)"
    if "app_xv_cache_invalidate()" not in heal:
        return False, "app_ledger_heal: GB ORIGINAL cache not invalidated after a heal (#389)"
    if not re.search(r"app_ledger_heal\(scratch\)", boot):
        return False, "app_ledger_heal_boot: does not call app_ledger_heal (#389)"
    i_h, i_m = load.find("app_ledger_heal(mig)"), load.find("xr_migrate_once(")
    if i_h < 0 or i_m < 0 or i_h > i_m:
        return False, "Gen-3 load: the ledger heal must run BEFORE xr_migrate_once (#389)"
    i_h, i_o = walk.find("app_ledger_heal(rb->sidecar)"), walk.find("f_opendir(&dir, PDNA_XFER_DIR)")
    if i_h < 0 or i_o < 0 or i_h > i_o:
        return False, "xfer_reconcile_walk: the heal must run BEFORE the xfer dir scan (#389)"
    if main.count("app_ledger_heal_boot();") != 1:
        return False, "main: the shipped boot must call app_ledger_heal_boot() exactly once (#389)"
    if dmain.count("app_ledger_heal_boot();") != 1:
        return False, "main (delta branch): the vsd boot must call app_ledger_heal_boot() exactly once (#389)"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #390
def f390(bridge: str, g3run: str, fill: str) -> tuple[bool, str]:
    if not re.search(r"if \(GB_GEN2 == dst_gen && fill_no_rom\) gb_gen12_norom_msg\(GB_GEN2\);\s*else gb_gen12_nomoves_msg\(dst_gen\);", bridge):
        return False, "gb_bank_down_bridge: a Gen-2 no-ROM refusal must say NO GEN-2 ROM (#390)"
    if not re.search(r"if \(g_ed->s\.gen == GB_GEN2 && fill_no_rom\) gb_gen12_norom_msg\(GB_GEN2\);\s*else gb_gen12_nomoves_msg\(g_ed->s\.gen\);", g3run):
        return False, "the bank-down->g3 zero-move refusal must say NO GEN-2 ROM for a Gen-2 session without a ROM (#390)"
    if "if (no_rom) *no_rom = !have_rom;" not in fill:
        return False, "gb_paste_fill_moves: no_rom out-flag not set from have_rom (#390)"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #387
def f387(detail: str, cls: str, tf: str, layout: str) -> tuple[bool, str]:
    if not re.search(r"case XRC_ABROAD_BANK:\s*return PDNA_XRC_D_ABROAD_BANK;", detail):
        return False, "xrc_detail_line: XRC_ABROAD_BANK has no Bank detail line (#387)"
    if not re.search(r"bank_g3_matches >= 1 && in->g3_key_matches == 0\) \{ out->kind = XRC_ABROAD_BANK;", cls):
        return False, "xrc_classify: a Bank-parked Gen-3 copy must classify XRC_ABROAD_BANK (#387)"
    if "PF(PDNA_XRC_D_ABROAD_BANK," not in tf:
        return False, "textfit: PDNA_XRC_D_ABROAD_BANK is not pinned (#387)"
    if '"In the Bank, restorable."' not in layout:
        return False, "pdna_layout.h: the Bank detail wording changed (#387)"
    return True, "ok"


def run() -> None:
    main_t = (SRC / "pdna_main.c").read_text()
    g12_t = (SRC / "pdna_gen12.c").read_text()
    # -------- #389
    heal = function_body(main_t, "app_ledger_heal")
    boot = function_body(main_t, "app_ledger_heal_boot")
    load = main_t  # the load site is inside a long function; check against the raw text window below
    m = re.search(r"uint8_t\* mig = app_box_swap_acquire\(GBSC_FILE_MAX\);.*?app_box_swap_release\(\);", strip_comments(main_t), re.S)
    load = m.group(0) if m else ""
    walk = function_body(main_t, "xfer_reconcile_walk")
    mn = strip_comments(main_t)
    k = mn.find("int main(void)")
    mainb = mn[k:] if k >= 0 else ""
    dk = mainb.find("vsd_attach()")
    dmain = mainb[dk:mainb.find("emulator build", dk)] if dk >= 0 else ""
    # the shipped boot is everything after the delta branch's closing comment
    smain = mainb[mainb.find("emulator build"):] if "emulator build" in mainb else ""
    parts = dict(heal=heal, boot=boot, load=load, walk=walk, main=smain, dmain=dmain)
    for k_, v in parts.items():
        check(bool(v), f"#389: {k_} not located")
    ok, d = f389(**parts)
    check(ok, d)
    run_muts("389", f389, parts, (
        ("xfer dir heal write gate dropped", "heal", "app_can_edit(), &ro1", "true, &ro1"),
        ("legacy dir not healed", "heal", "xh_heal_dir(PDNA_SIDECAR_DIR", "xh_heal_dir(PDNA_XFER_DIR"),
        ("rumble not paused", "heal", "rmbl_pause();", ""),
        ("cache not invalidated", "heal", "app_xv_cache_invalidate();", ""),
        ("boot wrapper does not heal", "boot", "app_ledger_heal(scratch);", ""),
        ("load heal after the migration", "load", "app_ledger_heal(mig);", "xr_migrate_once(mig, GBSC_FILE_MAX); app_ledger_heal(mig);"),
        ("walk heal removed", "walk", "app_ledger_heal(rb->sidecar);", ""),
        ("shipped boot call removed", "main", "app_ledger_heal_boot();", ""),
        ("vsd boot call removed", "dmain", "app_ledger_heal_boot();", ""),
    ))
    # -------- #390
    bridge = function_body(g12_t, "gb_bank_down_bridge")
    g3run = function_body(g12_t, "bank_down_g3_run")
    if not g3run:
        # the second site lives in the bank-down->g3 landing; locate by its log line
        m2 = re.search(r"[^\n]*\n(?:(?!\n\}\n).)*?bank-down->g3 moves.*?\n\}\n", strip_comments(g12_t), re.S)
        g3run = m2.group(0) if m2 else ""
    fill = function_body(g12_t, "gb_paste_fill_moves")
    parts = dict(bridge=bridge, g3run=g3run, fill=fill)
    for k_, v in parts.items():
        check(bool(v), f"#390: {k_} not located")
    ok, d = f390(**parts)
    check(ok, d)
    run_muts("390", f390, parts, (
        ("bridge: Gen-2 no-ROM says NO MOVES again", "bridge", "GB_GEN2 == dst_gen && fill_no_rom", "false && fill_no_rom"),
        ("g3 landing: Gen-2 no-ROM says NO MOVES again", "g3run", "g_ed->s.gen == GB_GEN2 && fill_no_rom", "false && fill_no_rom"),
        ("fill never reports a missing ROM", "fill", "if (no_rom) *no_rom = !have_rom;", ""),
    ))
    # -------- #387
    detail = function_body(main_t, "xrc_detail_line")
    cls = function_body((SRC / "xfer_reconcile.c").read_text(), "xrc_classify")
    tf = (ROOT / "tests" / "host_textfit_test.c").read_text()
    lay = (SRC / "pdna_layout.h").read_text()
    parts = dict(detail=detail, cls=cls, tf=tf, layout=lay)
    for k_, v in parts.items():
        check(bool(v), f"#387: {k_} not located")
    ok, d = f387(**parts)
    check(ok, d)
    run_muts("387", f387, parts, (
        ("detail line unwired", "detail", "case XRC_ABROAD_BANK:", "case XRC_G3HOME_DUP:"),
        ("classify back to the PC row", "cls", "out->kind = XRC_ABROAD_BANK;", "out->kind = XRC_ABROAD;"),
        ("pin dropped", "tf", "PF(PDNA_XRC_D_ABROAD_BANK,", "PF(X,"),
        ("wording says PC again", "layout", '"In the Bank, restorable."', '"In the Gen-3 PC, restorable."'),
    ))


def main() -> int:
    run()
    print(f"host_za2_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
