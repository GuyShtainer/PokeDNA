#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_zw_sites_test.py -- lane zw (BACKLOG #365-#369): wiring pins with teeth.

Each pin is a text check over comment-stripped function bodies; every pin is re-run against a MUTANT copy of
the real source (the fix reverted) and must FAIL there:
  Z365 gb_down_loss_screen shows the name-loss row
  Z369b ADD TEAM rows start below the n/6 header
  Z367d/Z367d6: daycare flavour call site passes the base row; no-ROM refusal reuses the Bank twin message
  Z367 gbdc_land: on Gen 1 the party leg uses gbs_insert_party() with a ROM base row, never gbs_move() box->party
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
fails: list[str] = []
n_checks = 0


def check(name, ok, detail=""):
    global n_checks
    n_checks += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"  [{detail}]"))
    if not ok:
        fails.append(name)


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def body(text, fn):
    t = strip_comments(text)
    m = re.search(r"(?m)^[A-Za-z_][^;{}\n]*\b" + re.escape(fn) + r"\s*\([^;{]*\)\s*\{", t)
    if not m:   # return type / attributes on the previous line: "static bool ...\nfn(args) {"
        m = re.search(r"(?m)^" + re.escape(fn) + r"\s*\([^;{]*\)\s*\{", t)
    if not m:
        return ""
    i, d = m.end() - 1, 0
    for j in range(i, len(t)):
        d += (t[j] == "{") - (t[j] == "}")
        if d == 0:
            return t[i:j + 1]
    return ""


def rd(n):
    return (SRC / n).read_text(errors="replace")


# ---- pins: name -> (file, function, predicate over the body) -----------------------------------------------
def p367(b):
    g1 = b.find("GB_GEN1")
    ip = b.find("gbs_insert_party_daycare(s, mon, &g1base,")
    mv = b.find("gbs_move(")
    return (g1 >= 0 and 0 <= ip < mv and g1 < ip and "gb12_gen1_base_for(" in b
            and "GBS_ERR_NEEDS_BASE" in b)


def p367d6(b):   # D6: the no-ROM refusal is the Bank twin's message, with the NO_ROM / BAD split
    return ("GB12_BASE_NO_ROM" in b and "gb_gen12_norom_msg(GB_GEN1)" in b and "GB12_BASE_BAD" in b
            and "PDNA_GBEDIT_MOVE_NEEDSBASE_L2" in b
            and 0 <= b.find("gb_rollback();") < b.find("gb_gen12_norom_msg("))   # rollback BEFORE the message


def p369(b):
    # mon rows start below the "%d/6 mons" header (y=18, 7 px tall) and the last row (6th) stays above the y=96 rule
    m = re.search(r"ui_text\(6,\s*(\d+)\s*\+\s*i\s*\*\s*10", b)
    return bool(m) and int(m.group(1)) >= 26 and int(m.group(1)) + 5 * 10 + 7 <= 95


def p365(b):
    k = b.find("down_name_text(n)")
    return (k >= 0 and "n->nick_lossy" in b and "n->otname_lossy" in b
            and b.find("PID search relaxed") < k < b.find("IVs come from DVs"))


def p366a(b):   # COUNTERS tab: the partial path repaints the CURRENT row even when sel did not move
    i = b.find("ctr_row_repaint(s, g, ctr_rows, ctr_n, top, sel, sel)")
    j = b.find("sel != c_sel")
    return i >= 0 and j >= 0 and "else if (sel != c_sel)" not in b


def p366b(b):   # trainer plain page: same
    i = b.find("gbtr_row_paint(t, rows[sel], gen1, row_y0 + sel * 9, true)")
    return i >= 0 and "} else if (sel != pv->sel)" not in b


def p366c(b):   # a rename re-encodes name_raw, which both real cards draw
    return bool(re.search(r"if \(renamed\)[^;]*gb_name_encode\([^;]*t->name_raw", b)) and b.find("strcpy(t->name, b)") < b.find("gb_name_encode(")


PINS = [
    ("Z366a COUNTERS partial repaint always redraws the current row", "pdna_gbflags.c", "pdna_gbflags", p366a),
    ("Z366b trainer plain page always redraws the current row", "pdna_gbtrainer.c", "gbtr_plain_render", p366b),
    ("Z366c rename re-encodes name_raw (Gen-1 + Gen-2 card face)", "pdna_gbtrainer.c", "gbtr_edit_row", p366c),
    ("Z365 DOWN confirm has a nickname/OT loss row (Gb12Notes flags)", "pdna_gen12.c", "gb_down_loss_screen", p365),
    ("Z369b ADD TEAM mon rows clear of the n/6 header", "pdna_gbhof.c", "hof_add_team_row_paint", p369),
    ("Z367 Gen-1 Day-Care party leg = gbs_insert_party + ROM base row", "pdna_gbdaycare.c", "gbdc_land", p367),
    ("Z367d D1 call site passes the ROM base row to the daycare flavour", "pdna_gbdaycare.c", "gbdc_land", p367),
    ("Z367d6 Day-Care no-ROM refusal = Bank twin message (NO_ROM vs BAD)", "pdna_gbdaycare.c", "gbdc_take", p367d6)]

# mutants: (pin name, file, old, new) -- applied to the REAL source text, the pin must go RED
MUTANTS = [
    ("Z366a COUNTERS partial repaint always redraws the current row", "pdna_gbflags.c",
     "      } else {                                    /* partial: swap", "      } else if (sel != c_sel) {                  /* partial: swap"),
    ("Z366b trainer plain page always redraws the current row", "pdna_gbtrainer.c",
     "  } else {\n    /* #366 D2", "  } else if (sel != pv->sel) {\n    /* #366 D2"),
    ("Z366c rename re-encodes name_raw (Gen-1 + Gen-2 card face)", "pdna_gbtrainer.c",
     "if (renamed) (void)gb_name_encode(", "if (0) (void)gb_name_encode("),
    ("Z365 DOWN confirm has a nickname/OT loss row (Gb12Notes flags)", "pdna_gen12.c",
     "y = loss_row(y, n->nick_lossy || n->otname_lossy, down_name_text(n));", "y = loss_row(y, false, down_name_text(n));"),
    ("Z365 DOWN confirm has a nickname/OT loss row (Gb12Notes flags)", "pdna_gen12.c",
     "n->nick_lossy || n->otname_lossy, down_name_text(n)", "n->nick_lossy, down_name_text(n)"),
    ("Z369b ADD TEAM mon rows clear of the n/6 header", "pdna_gbhof.c", "ui_text(6, 30 + i * 10", "ui_text(6, 20 + i * 10"),
    ("Z367 Gen-1 Day-Care party leg = gbs_insert_party + ROM base row", "pdna_gbdaycare.c",
     "gbs_insert_party_daycare(s, mon, &g1base, &pslot, list)", "gbs_insert(s, 0, mon, &pslot, list)"),
    ("Z367d D1 call site passes the ROM base row to the daycare flavour", "pdna_gbdaycare.c",
     "gbs_insert_party_daycare(s, mon, &g1base, &pslot, list)", "gbs_insert_party_daycare(s, mon, NULL, &pslot, list)"),
    ("Z367d6 Day-Care no-ROM refusal = Bank twin message (NO_ROM vs BAD)", "pdna_gbdaycare.c",
     "gb_gen12_norom_msg(GB_GEN1);\n    } else if", "msg_wait(0,0,0,0);\n    } else if"),
    ("Z367 Gen-1 Day-Care party leg = gbs_insert_party + ROM base row", "pdna_gbdaycare.c",
     "if (s->gen == GB_GEN1) {\n      /* #367", "if (0) {\n      /* #367"),
]


def main():
    srcs = {}
    for name, f, fn, pred in PINS:
        srcs.setdefault(f, rd(f))
        b = body(srcs[f], fn)
        check(name, bool(b) and pred(b), "body missing" if not b else "predicate false")
    for name, f, old, new in MUTANTS:
        pin = next(p for p in PINS if p[0] == name)
        t = srcs[f]
        check(f"mutant applies: {old[:40]!r}", old in t)
        b = body(t.replace(old, new, 1), pin[2])
        check(f"mutant RED: {name} / {new[:30]!r}", not (b and pin[3](b)))
    print(f"{n_checks} checks, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
