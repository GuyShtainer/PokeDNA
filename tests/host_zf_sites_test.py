#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zf_sites_test.py -- source pins for BACKLOG #416 (lane zf): the set-aside's replace-older log line and the
stuck-.tmp.bad refusal that NAMES the file. Each pin carries a self-mutation that must turn it red.

    python3 tests/host_zf_sites_test.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEAL = (ROOT / "source" / "xfer_heal.c").read_text()
GEN12 = (ROOT / "source" / "pdna_gen12.c").read_text()
LAYOUT = (ROOT / "source" / "pdna_layout.h").read_text()
MAIN = (ROOT / "source" / "pdna_main.c").read_text()


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


def checks(heal, gen12, layout, main=None):
    out = []
    if main is not None:
        # review-zf F1: GB_RECON_NAME_MAX grew 21 -> 25 for the .tmp form; the Gen-3 load walk must keep its own 20-char .pds bound
        rw = body(main, "gb_reconcile_walk")
        if "if (L < 5 || L >= 21) continue;" not in rw:
            out.append("F1: gb_reconcile_walk must filter .pds names at the literal 21, not GB_RECON_NAME_MAX")
        # reverify-zf F-A (#419: re-aimed): the sibling TRANSFERS walk's .pds bound (literal 21; only the .tmp branch uses
        # NAME_MAX 25) now lives in the pure xrc_walk_admit, host-proven by host_xfer_reconcile_test.c ADMIT-1 (19/21-char
        # names SKIP, mutants shown RED). What stays pinned HERE is that the walk really routes every name through it and
        # kept no private length filter of its own.
        xw = body(main, "xfer_reconcile_walk")
        if "xrc_walk_admit(fi.fname" not in xw:
            out.append("F-A: xfer_reconcile_walk must decide .pds/draft admission through xrc_walk_admit (#419)")
        if re.search(r"L\s*>=\s*(21|GB_RECON_NAME_MAX)", xw):
            out.append("F-A: xfer_reconcile_walk grew a private .pds length filter beside xrc_walk_admit (#419)")
    sa = body(heal, "xh_set_aside_tmp")
    if not re.search(r'f_stat\(bad, 0\) == FR_OK\)[^;]*\n?\s*log_line\("xfer_heal: %s: an older \.tmp\.bad salvage exists and is being replaced', sa):
        out.append("#416(a): xh_set_aside_tmp no longer logs the older .tmp.bad being replaced")
    li, ui = sa.find('log_line("xfer_heal: %s: an older'), sa.find("f_unlink(bad)")
    if li < 0 or ui < 0 or li > ui:
        out.append("#416(a): the replace log must come BEFORE the f_unlink(bad)")
    if "f_unlink(bad)" not in sa:
        out.append("#416(a): the deliberate overwrite-older policy (f_unlink(bad)) is gone")
    ar = body(heal, "xh_absent_resolve_ex")
    if not re.search(r"FRESULT sr = xh_set_aside_tmp\(path\);\s*if \(sr != FR_OK\) \{ if \(stuck\) \*stuck = \(sr == FR_EXIST\); return false; \}", ar):
        out.append("#416(b): xh_absent_resolve_ex must set *stuck exactly when the set-aside fails")
    # #419 S2: *stuck is raised ONLY by the failed set-aside (never by the primary-present refusal or any other return)
    assigns = re.findall(r"\*stuck\s*=\s*([^;]+);", ar)
    if assigns != ["false", "(sr == FR_EXIST)"]:
        out.append("#419(S2): xh_absent_resolve_ex must assign *stuck exactly twice (init false; set-aside failure == FR_EXIST), got %r" % assigns)
    pm = re.search(r"if \(hr == XH_NONE\) \{[^\n]*\}", ar)
    if not pm or "stuck" in pm.group(0):
        out.append("#419(S2): the primary-present refusal (hr == XH_NONE) must not touch *stuck")
    g = body(gen12, "gb_ledger_absent_heal")
    # #419 S6: the name shown is the BASENAME + ".tmp.bad" -- the '/' strip loop feeds the memcpy
    if not re.search(r"for \(const char\* q = path; \*q; q\+\+\) if \(\*q == '/'\) base = q \+ 1;", g) or \
       not re.search(r"memcpy\(nm, base, bn\)", g):
        out.append("#419(S6): gb_ledger_absent_heal must strip the directory (base) before building <key>.pds.tmp.bad")
    if "xh_absent_resolve_ex(" not in g or not re.search(r"if \(stuck\)", g):
        out.append("#416(b): gb_ledger_absent_heal does not branch on stuck")
    if not re.search(r'memcpy\(nm \+ bn, "\.tmp\.bad", 9\)', g) or "msg_wait(PDNA_SIDECAR_READFAIL_TITLE, UI_WARN, nm, PDNA_SIDECAR_STUCK_L2)" not in g:
        out.append("#416(b): the refusal does not show the <key>.pds.tmp.bad name")
    if '#define PDNA_SIDECAR_STUCK_L2' not in layout:
        out.append("#416(b): PDNA_SIDECAR_STUCK_L2 missing")
    return out


def main():
    v = checks(HEAL, GEN12, LAYOUT, MAIN)
    if v:
        for x in v:
            print("FAIL:", x)
        return 1
    print("ok: #416 (a) replace-older log line before the unlink, (b) stuck flag + named refusal; F1 gb_reconcile_walk + F-A xfer_reconcile_walk .pds bounds")
    fails = 0
    muts = (
        ("F1 bound widened", "main", "if (L < 5 || L >= 21) continue;", "if (L < 5 || L >= GB_RECON_NAME_MAX) continue;"),
        ("F-A xfer walk bypasses the helper", "main", "switch (xrc_walk_admit(fi.fname, (fi.fattrib & AM_DIR) != 0, ro_walk && pass == 0, &name_key, prim)) {", "switch (xrc_walk_admit_x(fi.fname, (fi.fattrib & AM_DIR) != 0, ro_walk && pass == 0, &name_key, prim)) {"),
        ("F-A xfer walk private length filter", "main", "    examined++;\n    uint64_t name_key = 0; bool draft = false;", "    examined++; int L = 0; while (fi.fname[L]) L++; if (L < 5 || L >= 21) continue;\n    uint64_t name_key = 0; bool draft = false;"),
        ("S2 stuck set on primary-present refusal", "heal", "if (ps != FR_NO_FILE && ps != FR_NO_PATH) return false; }", "if (ps != FR_NO_FILE && ps != FR_NO_PATH) { if (stuck) *stuck = true; return false; } }"),
        ("S6 basename not stripped", "gen12", "for (const char* q = path; *q; q++) if (*q == '/') base = q + 1;", ""),
        ("(a) log removed", "heal", 'log_line("xfer_heal: %s: an older .tmp.bad salvage exists', 'log_line_x("xfer_heal: %s: an older .tmp.bad salvage exists'),
        ("(a) policy removed", "heal", "(void)f_unlink(bad);", ""),
        ("(b) stuck never set", "heal", "if (stuck) *stuck = (sr == FR_EXIST);", ""),
        ("(b) caller ignores stuck", "gen12", "  if (stuck) {   /* #416(b)", "  if (0) {   /* #416(b)"),
        ("(b) name dropped", "gen12", 'msg_wait(PDNA_SIDECAR_READFAIL_TITLE, UI_WARN, nm, PDNA_SIDECAR_STUCK_L2)', 'msg_wait(PDNA_SIDECAR_READFAIL_TITLE, UI_WARN, "x", PDNA_SIDECAR_STUCK_L2)'),
    )
    for label, which, a, b in muts:
        srcs = {"heal": HEAL, "gen12": GEN12, "main": MAIN}
        if a not in srcs[which]:
            print(f"FAIL: mutation {label} target not found"); fails += 1; continue
        srcs[which] = srcs[which].replace(a, b, 1)
        if checks(srcs["heal"], srcs["gen12"], LAYOUT, srcs["main"]):
            print(f"mutation {label}: correctly caught")
        else:
            print(f"FAIL: mutation {label} did NOT turn a check red"); fails += 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
