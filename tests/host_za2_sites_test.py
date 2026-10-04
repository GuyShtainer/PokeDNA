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
    i_h, i_o = walk.find("app_ledger_heal(rb->sidecar)"), walk.find("f_opendir(&dir, pass == 0 ? PDNA_XFER_DIR : PDNA_SIDECAR_DIR)")
    if i_h < 0 or i_o < 0 or i_h > i_o:
        return False, "xfer_reconcile_walk: the heal must run BEFORE the xfer dir scan (#389)"
    if main.count("app_ledger_heal_boot();") != 1:
        return False, "main: the shipped boot must call app_ledger_heal_boot() exactly once (#389)"
    if dmain.count("app_ledger_heal_boot();") != 1:
        return False, "main (delta branch): the vsd boot must call app_ledger_heal_boot() exactly once (#389)"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #389 review D2
D2_SITES = (("main", "app_xfer_promote"), ("main", "app_xfer_pending_undo"), ("main", "app_xfer_pid_rekey"),
            ("main", "app_paste_gb_commit"), ("main", "xfer_reconcile_apply"),
            ("g12", "gb_paste_sidecar_undo"), ("g12", "xfer_down_undo"))


def f389u(**b: str) -> tuple[bool, str]:
    for k, body in b.items():
        if "xh_unlink_ledger(" not in body:
            return False, f"{k}: a ledger delete does not go through xh_unlink_ledger (a stale .tmp would resurrect it)"
        if re.search(r"\bf_unlink\(", body):
            return False, f"{k}: a bare f_unlink of a ledger path is back"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #389 review D4
def f389g(helper: str, paste: str, down: str) -> tuple[bool, str]:
    if "xh_absent_resolve_ex(PDNA_XFER_DIR," not in helper or "app_can_edit()" not in helper:
        return False, "gb_ledger_absent_heal must call xh_absent_resolve_ex(PDNA_XFER_DIR, ... app_can_edit() ...) (#389 D4)"
    if not re.search(r"SF_ERR_OPEN\)\s*\{\s*if \(!gb_ledger_absent_heal\(path, key, g_ed->sidecar, &len\)\) return false;", paste):
        return False, "gb_paste_write: the SF_ERR_OPEN branch does not heal before starting a fresh ledger (#389 D4)"
    if not re.search(r"SF_ERR_OPEN\)\s*\{\s*if \(!gb_ledger_absent_heal\(path_out, key, scratch, &len\)\) return -1;", down):
        return False, "xfer_down_write: the SF_ERR_OPEN branch does not heal before starting a fresh ledger (#389 D4)"
    if "gbsc_init" in paste.split("SF_ERR_OPEN")[1].split("else if")[0] or "gbsc_init" in down.split("SF_ERR_OPEN")[1].split("else if")[0]:
        return False, "a SF_ERR_OPEN branch still gbsc_init()s directly (#389 D4)"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #390
def f390(bridge: str, g3run: str, fill: str) -> tuple[bool, str]:
    if not re.search(r"if \(GB_GEN2 == dst_gen && fill_no_rom\) gb_gen12_norom_msg\(GB_GEN2\);\s*else gb_gen12_nomoves_msg\(dst_gen\);", bridge):
        return False, "gb_bank_down_bridge: a Gen-2 no-ROM refusal must say NO GEN-2 ROM (#390)"
    if not re.search(r"if \(g_ed->s\.gen == GB_GEN2 && fill_no_rom\) gb_gen12_norom_msg\(GB_GEN2\);\s*else gb_gen12_nomoves_msg\(g_ed->s\.gen\);", g3run):
        return False, "the bank-down->g3 zero-move refusal must say NO GEN-2 ROM for a Gen-2 session without a ROM (#390)"
    if "bool have_rom = located;" not in fill or "if (no_rom) *no_rom = !located;" not in fill:
        return False, "gb_paste_fill_moves: no_rom must mean 'no ROM LOCATED' (!located), not 'learnset missing' (#390 review D3)"
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


# ------------------------------------------------------------------------------------------------ #388
def f388(render: str, left: str, hint: str, leftfn: str, g3call: str, g1fn: str = "", main_t: str = "") -> tuple[bool, str]:
    if g1fn and not (re.search(r"t >= 0x07u && t <= 0x08u", g1fn) and re.search(r"t >= 0x14u && t <= 0x1Au", g1fn)):
        return False, "g1_to_g3_type: Gen-1 type ids are 0x00-0x05, 0x07-0x08, 0x14-0x1A only (0x09 STEEL / 0x1B DARK are glitch ids) (#388 D5)"
    if main_t and not re.search(r"g_src_ops->view\(rec\); return false; \}\s*if \(occupied\) \{ uint8_t d\[100\]; int card = 0; pdna_inspect", main_t):
        return False, "the read-only box menu path does not use the source's own view op before pdna_inspect (#388 D5)"
    if not re.search(r"if \(e->gen == GB_GEN1\) \{\s*t1o = g1_to_g3_type\(gb_get_gen1_type1\(e\)\);\s*t2o = \(t1o >= 0\) \? g1_to_g3_type\(gb_get_gen1_type2\(e\)\) : -1;", render):
        return False, "gbsummary render: a Gen-1 record's own type bytes are not handed to the left panel (#388)"
    if not re.search(r"pdna_summary_draw_left_hint\(left, false, e->gen, t1o, t2o\)", render):
        return False, "gbsummary render: the type override is not passed to pdna_summary_draw_left_hint (#388)"
    if not re.search(r"if \(t1o >= 0\) \{ t1 = \(uint8_t\)t1o; t2 = \(t2o >= 0\) \? \(uint8_t\)t2o : t1; \}", left):
        return False, "draw_left_ex: the type override is not applied before the badges (#388)"
    if "draw_left_ex(p, back, t1o, t2o);" not in hint:
        return False, "pdna_summary_draw_left_hint does not forward the override (#388)"
    if "draw_left_ex(p, g_back, -1, -1)" not in leftfn or "draw_left_ex(p, back, -1, -1)" not in g3call:
        return False, "a Gen-3 summary caller no longer passes -1/-1 (species table) (#388)"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #391
def f391(helper: str, save_now: str, flush_exit: str, layout: str, tf: str) -> tuple[bool, str]:
    if not re.search(r"kept == 1 \? PDNA_XFER_FLUSHFAIL_L1_ONE : PDNA_XFER_FLUSHFAIL_L1", helper) or \
       not re.search(r"kept == 1 \? PDNA_XFER_FLUSHFAIL_L2_ONE : PDNA_XFER_FLUSHFAIL_L2", helper):
        return False, "app_flushfail_msg: not singular/plural on both lines (#391a)"
    if "app_flushfail_msg(kept);" not in save_now or "app_flushfail_msg(kept);" not in flush_exit:
        return False, "a BANK NOT FULLY UPDATED call site bypasses app_flushfail_msg (#391a)"
    if "PDNA_XFER_FLUSHFAIL_L1, kept" in save_now or "PDNA_XFER_FLUSHFAIL_L1, kept" in flush_exit:
        return False, "a call site still formats the plural-only line (#391a)"
    if '"Kept until you save. Load"' not in layout:
        return False, "PDNA_GBMAP_PLACED_L1 no longer says the placement is kept until the save (#391b)"
    if "PF(PDNA_GBMAP_PLACED_L1," not in tf or "PF(PDNA_XFER_FLUSHFAIL_L2_ONE," not in tf:
        return False, "textfit: the #391 strings are not pinned"
    return True, "ok"


# ------------------------------------------------------------------------------------------------ #386
def f386(cls: str, phase2: str, walk: str, classify_all: str, detail: str, bank_peek: str) -> tuple[bool, str]:
    if len(re.findall(r"in->bank_unread", cls)) < 2:
        return False, "xrc_classify: both LOST branches (PENDING + CLAIMED) must consult bank_unread (#386)"
    if "pdna_bank_peek_box_ex(box, &unread)" not in phase2 or not re.search(r"if \(!recs\) \{ if \(unread\) \{ rb->bank_unread = true;", phase2):
        return False, "phase 2: an unreadable (not merely ABSENT) Bank box must raise rb->bank_unread via peek_box_ex (#386 D1)"
    if "if (box_load(box)) { *unread = false; return box_recs(); }" not in bank_peek or "if (g_box_unread) return NULL;" not in bank_peek:
        return False, "pdna_bank_peek_box_ex: must report unread=false on a good load and leave unread set on a read error / failed heal (#386 D1)"
    if not re.search(r"f_stat\(p, 0\);\s*\*unread = !\(fr == FR_NO_FILE \|\| fr == FR_NO_PATH\);", bank_peek):
        return False, "pdna_bank_peek_box_ex: a failed load with g_box_unread 0 must f_stat the primary; only FR_NO_FILE/FR_NO_PATH is absent, a truncated box is unread (#393)"
    if not re.search(r"\*unread = true;\s+if \(box < 0 \|\| box >= BANK_BOXES\) return NULL;", bank_peek):
        return False, "pdna_bank_peek_box_ex: a bad box / refused flush must report unread (#386 D1)"
    if "rb->bank_unread = false;" not in walk:
        return False, "xfer_reconcile_walk: bank_unread is never reset per scan (#386)"
    if "in.bank_unread = rb->bank_unread;" not in classify_all:
        return False, "classify_all: the flag is not handed to xrc_classify (#386)"
    if not re.search(r"case XRC_UNREAD:\s*return PDNA_XRC_D_UNREAD;", detail):
        return False, "xrc_detail_line: XRC_UNREAD has no detail line (#386)"
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
    # -------- #389 review D2
    texts = {"main": main_t, "g12": g12_t}
    parts = {nm: function_body(texts[f], nm) for f, nm in D2_SITES}
    for k_, v in parts.items():
        check(bool(v), f"#389 D2: {k_} not located")
    ok, d = f389u(**parts)
    check(ok, d)
    run_muts("389u", f389u, parts, tuple(
        (f"{nm} back to a bare f_unlink", nm, "xh_unlink_ledger(", "f_unlink(") for _, nm in D2_SITES))
    # -------- #389 review D4
    parts = dict(helper=function_body(g12_t, "gb_ledger_absent_heal"), paste=function_body(g12_t, "gb_paste_write"),
                 down=function_body(g12_t, "xfer_down_write"))
    for k_, v in parts.items():
        check(bool(v), f"#389 D4: {k_} not located")
    ok, d = f389g(**parts)
    check(ok, d)
    run_muts("389g", f389g, parts, (
        ("helper does not heal", "helper", "xh_absent_resolve_ex(PDNA_XFER_DIR,", "xh_resolve_not(PDNA_XFER_DIR,"),
        ("helper ignores the write gate", "helper", "app_can_edit()", "true"),
        ("gb_paste_write inits directly", "paste", "if (!gb_ledger_absent_heal(path, key, g_ed->sidecar, &len)) return false;", "len = (uint32_t)gbsc_init(g_ed->sidecar, key);"),
        ("xfer_down_write inits directly", "down", "if (!gb_ledger_absent_heal(path_out, key, scratch, &len)) return -1;", "len = (uint32_t)gbsc_init(scratch, key);"),
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
        ("fill never reports a missing ROM", "fill", "if (no_rom) *no_rom = !located;", ""),
        ("D3: ROM present but learnset missing claims NO ROM", "fill", "*no_rom = !located;", "*no_rom = !have_rom;"),
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
    # -------- #388
    sm_t = (SRC / "pdna_summary.c").read_text()
    gs_t = (SRC / "pdna_gbsummary.c").read_text()
    # left panel body = draw_left_ex; the file-level statics are one-liners -> take them from the stripped text
    sm_s = strip_comments(sm_t)
    m = re.search(r"static void draw_left\(const PkMon\* p\) \{[^\n]*\}", sm_s)
    leftfn = m.group(0) if m else ""
    m = re.search(r"void pdna_summary_draw_left\(const PkMon\* p, bool back\) \{[^\n]*\}", sm_s)
    g3call = m.group(0) if m else ""
    parts = dict(render=function_body(gs_t, "render"), left=function_body(sm_t, "draw_left_ex"),
                 hint=function_body(sm_t, "pdna_summary_draw_left_hint"), leftfn=leftfn, g3call=g3call,
                 g1fn=function_body(gs_t, "g1_to_g3_type"), main_t=strip_comments(main_t))
    for k_, v in parts.items():
        check(bool(v), f"#388: {k_} not located")
    ok, d = f388(**parts)
    check(ok, d)
    run_muts("388", f388, parts, (
        ("Gen-1 override dropped", "render", "if (e->gen == GB_GEN1) {", "if (false) {"),
        ("override not passed", "render", "e->gen, t1o, t2o)", "e->gen, -1, -1)"),
        ("override not applied", "left", "if (t1o >= 0) {", "if (false) {"),
        ("hint drops the override", "hint", "draw_left_ex(p, back, t1o, t2o);", "draw_left_ex(p, back, -1, -1);"),
        ("a Gen-3 caller passes an override", "leftfn", "g_back, -1, -1", "g_back, 0, 0"),
        ("D5: STEEL accepted again", "g1fn", "t <= 0x08u", "t <= 0x09u"),
        ("D5: DARK accepted again", "g1fn", "t <= 0x1Au", "t <= 0x1Bu"),
        ("D5: RO box view drops the view op", "main_t", "g_src_ops->view(rec); return false; }   \n    if (occupied) { uint8_t d[100]; int card = 0; pdna_inspect", "return false; }\n    if (occupied) { uint8_t d[100]; int card = 0; pdna_inspect"),
    ))
    # -------- #391
    parts = dict(helper=function_body(main_t, "app_flushfail_msg"), save_now=function_body(main_t, "app_xfer_save_now"),
                 flush_exit=function_body(main_t, "flush_on_exit"), layout=lay, tf=tf)
    for k_, v in parts.items():
        check(bool(v), f"#391: {k_} not located")
    ok, d = f391(**parts)
    check(ok, d)
    run_muts("391", f391, parts, (
        ("singular dropped (line 1)", "helper", "kept == 1 ? PDNA_XFER_FLUSHFAIL_L1_ONE : PDNA_XFER_FLUSHFAIL_L1", "PDNA_XFER_FLUSHFAIL_L1"),
        ("singular dropped (line 2)", "helper", "kept == 1 ? PDNA_XFER_FLUSHFAIL_L2_ONE : PDNA_XFER_FLUSHFAIL_L2", "PDNA_XFER_FLUSHFAIL_L2"),
        ("save-now bypasses the helper", "save_now", "app_flushfail_msg(kept);", "siprintf(l1, PDNA_XFER_FLUSHFAIL_L1, kept);"),
        ("exit flush bypasses the helper", "flush_exit", "app_flushfail_msg(kept);", "(void)kept;"),
        ("PLACED wording reverted", "layout", '"Kept until you save. Load"', '"Load your save to appear"'),
        ("pin dropped", "tf", "PF(PDNA_GBMAP_PLACED_L1,", "PF(X,"),
    ))
    # -------- #386
    parts = dict(cls=cls, phase2=function_body(main_t, "xfer_reconcile_bank_phase2"), walk=function_body(main_t, "xfer_reconcile_walk"),
                 classify_all=function_body(main_t, "xfer_reconcile_classify_all"), detail=detail,
                 bank_peek=function_body((SRC / "pdna_bank.c").read_text(), "pdna_bank_peek_box_ex"))
    for k_, v in parts.items():
        check(bool(v), f"#386: {k_} not located")
    ok, d = f386(**parts)
    check(ok, d)
    run_muts("386", f386, parts, (
        ("PENDING branch ignores the flag", "cls", "else if (in->bank_unread)", "else if (false)"),
        ("CLAIMED branch ignores the flag", "cls", "!g3_seen && in->bank_unread)", "!g3_seen && false)"),
        ("phase 2 does not raise it", "phase2", "rb->bank_unread = true;", ""),
        ("absent box raises the flag (D1 bug)", "phase2", "if (unread) { rb->bank_unread = true;", "{ rb->bank_unread = true;"),
        ("phase 2 back on plain peek (D1 bug)", "phase2", "pdna_bank_peek_box_ex(box, &unread)", "pdna_bank_peek_box(box)"),
        ("walk never resets it", "walk", "rb->bank_unread = false;", ""),
        ("classify_all drops it", "classify_all", "in.bank_unread = rb->bank_unread;", ""),
        ("detail line unwired", "detail", "case XRC_UNREAD:", "case XRC_G3HOME_DUP:"),
        ("peek helper always reports unread", "bank_peek", "*unread = !(fr == FR_NO_FILE || fr == FR_NO_PATH);", "*unread = true;"),
        ("#393 truncated box reads absent (old behaviour)", "bank_peek", "*unread = !(fr == FR_NO_FILE || fr == FR_NO_PATH);", "*unread = false;"),
        ("#393 only FR_NO_FILE counts absent", "bank_peek", "fr == FR_NO_FILE || fr == FR_NO_PATH", "fr == FR_NO_FILE"),
        ("#393 read-error early return dropped", "bank_peek", "if (g_box_unread) return NULL;", ""),
        ("peek helper leaves unread set on a good load", "bank_peek", "*unread = false; return box_recs();", "return box_recs();"),
        ("peek helper defaults to read", "bank_peek", "*unread = true;\n  if (box < 0", "*unread = false;\n  if (box < 0"),
    ))


def main() -> int:
    run()
    print(f"host_za2_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
