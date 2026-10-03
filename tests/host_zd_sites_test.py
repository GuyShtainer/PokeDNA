#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zd_sites_test.py -- lane zd structural pins (pure text, no build).

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


# ---- #404 the dex grid page pass is bracketed by ONE gb_art_batch (begin ... cells ... end)
pick = rd("pdna_pick.c")
PASS = (r"GbArtBatch bt;\s*bool batched = dex_cell_art_page_served\(\);\s*"
        r"if \(batched\) gb_art_batch_begin\(&bt\);\s*"
        r"for \(int i = 0; i < vis && top \+ i < g_n; i\+\+\)\s*"
        r"dex_cell_grid\([^;]*\);\s*"
        r"if \(batched\) gb_art_batch_end\(&bt\);")
pin("#404 dex_grid_cells brackets the cell loop in begin/end, only for a page the GB override serves",
    pick, lambda t: re.search(PASS, t) is not None,
    lambda t: t.replace("if (batched) gb_art_batch_begin(&bt);", ""))
pin("#404 the batch is closed on the only path out (no return between begin and end)",
    pick, lambda t: re.search(PASS, t) is not None,
    lambda t: t.replace("if (batched) gb_art_batch_end(&bt);", ""))
pin("#404 the page repaint reaches the grid through dex_grid_cells (not a bare per-cell loop)",
    pick, lambda t: "if (grid) dex_grid_cells(x0, y0, cols, cw, ch, top, vis, bob);" in t,
    lambda t: t.replace("if (grid) dex_grid_cells(x0, y0, cols, cw, ch, top, vis, bob);",
                        "if (grid) for (int i = 0; i < vis && top + i < g_n; i++) dex_cell_grid(x0 + (i % cols) * cw, y0 + (i / cols) * ch, g_list[top + i], bob);"))
# ---- #410 the redo chord applies the offer's ja_cut_tail guard (never stops on a trailing unpaired older half)
jrn = rd("jrn_app.c")
pin("#410 ja_redo_in_cut refuses when EVERY remaining redo step is the cut tail (cut >= av), via ja_cut_tail",
    jrn,
    lambda t: re.search(r"static int __attribute__\(\(noinline\)\) ja_redo_in_cut\(void\) \{[^}]*?ja_cut_tail\(av, total, &cut\);[^}]*?return cut >= av \? 1 : 0;", t, re.S) is not None,
    lambda t: t.replace("return cut >= av ? 1 : 0;", "return 0;"))
pin("#410 jrnapp_step_pair consults the guard on a plain REDO (g < 2 && dir > 0) and returns JRN_NOOP",
    jrn,
    lambda t: re.search(r"if \(g < 2 && dir > 0\) \{.{0,200}?rc = ja_redo_in_cut\(\);.{0,300}?if \(rc > 0\) return JRN_NOOP;", t, re.S) is not None,
    lambda t: t.replace("rc = ja_redo_in_cut();", "rc = 0;"))
pin("#410 a read fault in the guard's walk refuses the press (never a half swap)",
    jrn,
    lambda t: re.search(r"if \(rc < 0\) \{ ja_event\(\"redo: a read fault in the half-swap guard, the press is refused\", rc\); return rc; \}", t) is not None,
    lambda t: t.replace("if (rc < 0) { ja_event(\"redo: a read fault in the half-swap guard, the press is refused\", rc); return rc; }", ""))
# ---- #392(b) step (1c) asks the pure guard BEFORE reading the sidecar / burning a serial, and drops the twin's entry
main = rd("pdna_main.c")
G = r"if \(h->action != XRC_ACT_RESTORE\) continue;\s*if \(xrc_restore_is_dup\(rb->xrc, remove_entry, i\)\) \{[^}]*remove_entry\[i\] = true;[^}]*continue;\s*\}\s*gb_recon_path"
pin("#392b step (1c) skips a RESTORE whose original was already restored this pass (guard before the read), entry dropped",
    main, lambda t: re.search(G, t) is not None,
    lambda t: t.replace("if (xrc_restore_is_dup(rb->xrc, remove_entry, i)) {", "if (0) {"))
pin("#392b the guard skip drops the twin's ledger entry (remove_entry) rather than leaving a row that re-offers RESTORE",
    main, lambda t: re.search(G, t) is not None,
    lambda t: t.replace("      remove_entry[i] = true;\n      log_line(\"xfer: reconcile: row %d RESTORE skipped", "      log_line(\"xfer: reconcile: row %d RESTORE skipped"))

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("host_zd_sites_test: all pins green on source and red on mutants")
