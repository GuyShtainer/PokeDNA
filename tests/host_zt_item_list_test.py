#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_zt_item_list_test.py -- BACKLOG #356/#357: the Gen-2 item-picker LISTS, run for real.

source/pdna_pick.c does not compile on the host (tonc UI), so this test EXTRACTS the real text of
item_build() and item_held_row() (brace-matched, verbatim) plus the restricted category tables, drops
them into a small C harness with stubs for the UI-only neighbours, links the real gb_bag.c /
gb_item_names.c, and asserts on the lists they build:

  A  the 25 unused ids (06 19 2D 32 38 5A 64 78 87 88 89 8D 8E 91 93 94 95 99 9A 9B A2 AB B0 B3 BE) are in NO
     list -- held mode and bag/pack ADD mode, Gold and Crystal, every category;
  B  every id that does appear has a real name (gb_item_label), and every pocket-valid named id appears;
  C  the four Crystal-only ids (CLEAR BELL 46, GS BALL 73, BLUE CARD 74, EGG TICKET 81) are listed for
     GBF_G_CRYSTAL and not for GBF_G_GS (this is what #356's game threading buys);
  D  held mode keeps the NO ITEM row (id 0, first); ADD mode never has it;
  E  item_held_row: a held byte outside the list gets ONE preselected row in id order (held mode only);
     a listed byte, 0, and ADD mode leave the list untouched;
  F  exact per-category counts (Gold/Crystal 0..4 add+held; Red AND Yellow All/Items/TM-HM add+held);
  G  after item_held_row's insert the idx[] list is strictly ascending, the planted hole 0x06 at row 6.

Every assertion is shown RED against a mutant of the REAL extracted source, every run.
Run: python3 tests/host_zt_item_list_test.py   (registered in tests/run_host_tests.py PY_TESTS)
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
HOLES = [0x06, 0x19, 0x2D, 0x32, 0x38, 0x5A, 0x64, 0x78, 0x87, 0x88, 0x89, 0x8D, 0x8E, 0x91, 0x93, 0x94, 0x95,
         0x99, 0x9A, 0x9B, 0xA2, 0xAB, 0xB0, 0xB3, 0xBE]
CRYSTAL_ONLY = [0x46, 0x73, 0x74, 0x81]
OBJS = ["gb_bag.c", "gb_item_names.c", "gb_fields.c", "gb_session.c", "gb_edit.c", "gen1_save.c", "gen1_write.c",
        "gen2_save.c", "gen2_write.c", "data_tables.c", "item_map_g2g3.c", "item_map_g1g2.c", "gen3_to_gb.c",
        "gb_sidecar.c", "bank_cell.c", "gen3_edit.c", "gen3_mon.c", "gen3_box.c", "gen3_save.c", "gen3_daycare.c"]


def func(text: str, sig: str) -> str:
    i = text.index(sig)
    j = text.index("{", i)
    d = 0
    for k in range(j, len(text)):
        d += text[k] == "{"
        d -= text[k] == "}"
        if d == 0:
            return text[i:k + 1]
    raise SystemExit("unbalanced " + sig)


def extract(pick_c: str) -> str:
    a = pick_c.index("#define NRICAT_G1")
    b = pick_c.index("};", pick_c.index("RICAT_POCKET_G2[NRICAT_G2]")) + 2
    return "\n".join([
        func(pick_c, "static bool ci_contains"), func(pick_c, "static char up1") if "static char up1" in pick_c else "",
        func(pick_c, "static bool num_prefix"), func(pick_c, "static void item_label_for"),
        pick_c[a:b], func(pick_c, "static int item_build"), func(pick_c, "static int item_held_row")])


HARNESS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "gb_bag.h"
#include "gb_item_names.h"
typedef uint16_t u16;
#define siprintf sprintf
#define NITEM 400
static int g_item_max_id, g_item_gen, g_item_game, g_item_allow_none;
static const int8_t ICAT_POCKET[6] = {-1,0,0,0,0,0};
static int pk_item_pocket(uint16_t i) { (void)i; return 0; }
static int pk_item_games(uint16_t i) { (void)i; return 0; }
static const char* pk_item_name(uint16_t i) { (void)i; return ""; }
static void pk_item_label(uint16_t i, char* o, int c) { (void)i; (void)c; o[0] = 0; }
static char up1(char c);
@@EXTRACT@@
static void dump(const char* tag, u16* idx, int n) {
  printf("%s", tag);
  for (int i = 0; i < n; i++) printf(" %u", idx[i]);
  printf("\n");
}
int main(void) {
  static u16 idx[NITEM];
  int games[2] = { GBF_G_GS, GBF_G_CRYSTAL };
  const char* gname[2] = { "GS", "CRYSTAL" };
  for (int g1 = 0; g1 < 2; g1++)
    for (int held = 0; held < 2; held++)
      for (int cat = 0; cat < NRICAT_G1; cat++) {
        int gm = g1 ? GBF_G_YELLOW : GBF_G_RED;
        g_item_gen = GBIN_GEN1; g_item_game = gm; g_item_max_id = gbb_max_item_id((GbGame)gm);
        g_item_allow_none = held;
        int n = item_build(idx, "", 0, cat, 0);
        char tag[48]; sprintf(tag, "L %s %s %d:", g1 ? "YELLOW" : "RED", held ? "held" : "add", cat);
        dump(tag, idx, n);
      }
  for (int g = 0; g < 2; g++)
    for (int held = 0; held < 2; held++)
      for (int cat = 0; cat < NRICAT_G2; cat++) {
        g_item_gen = GBIN_GEN2; g_item_game = games[g]; g_item_max_id = gbb_max_item_id((GbGame)games[g]);
        g_item_allow_none = held;
        int n = item_build(idx, "", 0, cat, 0);
        char tag[48]; sprintf(tag, "L %s %s %d:", gname[g], held ? "held" : "add", cat);
        dump(tag, idx, n);
        for (int i = 0; i < n; i++) {
          char nm[32];
          if (idx[i] && !gb_item_label(GBIN_GEN2, (uint8_t)idx[i], nm, sizeof nm)) printf("UNNAMED %u\n", idx[i]);
        }
      }
  /* item_held_row: held mode, Gold; probe a hole, a listed id, 0, an over-range byte; then ADD mode */
  g_item_gen = GBIN_GEN2; g_item_game = GBF_G_GS; g_item_max_id = gbb_max_item_id(GBF_G_GS);
  unsigned probes[5] = { 0x06, 0x01, 0, 0xBE, 0xFF };
  for (int mode = 0; mode < 2; mode++) {
    g_item_allow_none = mode == 0;
    for (int p = 0; p < 5; p++) {
      int n = item_build(idx, "", 0, 0, 0), n0 = n;
      int sel = item_held_row(idx, &n, (uint16_t)probes[p], 0);
      int asc = 1;
      for (int q = 1; q < n; q++) if (idx[q] <= idx[q - 1]) asc = 0;
      printf("R %s %u n0=%d n=%d sel=%d selid=%u asc=%d\n", mode == 0 ? "held" : "add", probes[p], n0, n, sel, idx[sel], asc);
    }
  }
  return 0;
}
'''


def run(pick_c: str) -> dict:
    code = HARNESS.replace("@@EXTRACT@@", extract(pick_c))
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "h.c"
        c.write_text(code)
        exe = Path(td) / "h"
        cmd = ["cc", "-std=gnu11", "-w", "-I", str(SRC), str(c)] + [str(SRC / o) for o in OBJS] + ["-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-2000:])
            raise SystemExit("harness build failed")
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    res = {"lists": {}, "unnamed": [], "R": {}}
    for ln in out.splitlines():
        if ln.startswith("L "):
            head, ids = ln.split(":", 1)
            _, g, mode, cat = head.split()
            res["lists"][(g, mode, int(cat))] = [int(x) for x in ids.split()]
        elif ln.startswith("UNNAMED"):
            res["unnamed"].append(int(ln.split()[1]))
        elif ln.startswith("R "):
            m = re.match(r"R (\w+) (\d+) n0=(\d+) n=(\d+) sel=(\d+) selid=(\d+) asc=(\d+)", ln)
            res["R"][(m.group(1), int(m.group(2)))] = tuple(int(m.group(i)) for i in range(3, 8))
    return res


EXPECT = {   # (game, mode) -> exact list length per category (All, Items, Balls, Key, TM-HM / Gen 1: All, Items, TM-HM)
    ("GS", "add"): [218, 131, 12, 18, 57], ("GS", "held"): [219, 131, 12, 18, 57],
    ("CRYSTAL", "add"): [222, 131, 12, 22, 57], ("CRYSTAL", "held"): [223, 131, 12, 22, 57],
    ("RED", "add"): [150, 95, 55], ("RED", "held"): [151, 95, 55],
    ("YELLOW", "add"): [150, 95, 55], ("YELLOW", "held"): [151, 95, 55],
}
K_A = "A holes in no list (held+add, GS+Crystal, every category)"
K_E = "E hole byte -> one inserted preselected row"
K_E1 = "E' listed byte / 0 / add mode leave the list untouched"
K_E2 = "E'' another hole (BE) and over-range FF each get their own row"
K_F = "F exact per-category counts (GS/Crystal 0..4, Red/Yellow 0..2, add+held)"
K_G = "G idx ascending after the held-row insert; hole 0x06 at row 6"


def verdicts(res: dict) -> dict[str, bool]:
    L = res["lists"]
    v = {}
    v[K_A] = all(not (set(ids) & set(HOLES)) for (g, _m, _c), ids in L.items() if g in ("GS", "CRYSTAL"))
    v["A' the unfiltered lists are non-trivial (>100 ids)"] = all(len(L[(g, m, 0)]) > 100 for g in ("GS", "CRYSTAL") for m in ("held", "add"))
    v["B every listed id has a real name"] = not res["unnamed"]
    v["C Crystal-only 4 in Crystal held+add All, absent from Gold"] = all(
        x in L[("CRYSTAL", m, 0)] and x not in L[("GS", m, 0)] for x in CRYSTAL_ONLY for m in ("held", "add"))
    v["D held has NO ITEM (0) first; add never"] = all(L[(g, "held", 0)][0] == 0 for g in ("GS", "CRYSTAL")) and all(
        0 not in L[(g, "add", c)] for g in ("GS", "CRYSTAL") for c in range(5))
    R = res["R"]
    v[K_E] = R.get(("held", 0x06), (0,) * 5)[:4] == (R[("held", 0x06)][0], R[("held", 0x06)][0] + 1, R[("held", 0x06)][2], 6) and R[("held", 0x06)][2] > 0
    v[K_E1] = (
        R[("held", 0x01)][1] == R[("held", 0x01)][0] and R[("held", 0)][1] == R[("held", 0)][0]
        and all(R[("add", p)][1] == R[("add", p)][0] for p in (0x06, 0x01, 0, 0xBE, 0xFF)))
    v[K_E2] = (
        R[("held", 0xBE)][1] == R[("held", 0xBE)][0] + 1 and R[("held", 0xBE)][3] == 0xBE
        and R[("held", 0xFF)][1] == R[("held", 0xFF)][0] + 1 and R[("held", 0xFF)][3] == 0xFF)
    v[K_F] = all([len(L.get((g, m, c), [])) for c in range(len(cnt))] == cnt for (g, m), cnt in EXPECT.items())
    v[K_G] = (all(R[("held", p)][4] == 1 for p in (0x06, 0x01, 0, 0xBE, 0xFF))
              and R[("held", 0x06)][2] == 6)
    return v


K_D1 = "D held has NO ITEM (0) first; add never"
K_D2 = K_D1
K_C = "C Crystal-only 4 in Crystal held+add All, absent from Gold"


def main() -> int:
    pick_c = (SRC / "pdna_pick.c").read_text()
    bad = []
    real = verdicts(run(pick_c))
    print("real tree:")
    for k, ok in real.items():
        print(f"  {'ok  ' if ok else 'FAIL'} {k}")
        if not ok:
            bad.append(k)
    muts = [
        ("M-A the named-id filter removed", K_A, "      if (!(i == 0 && g_item_allow_none)) {\n        char nm0", "      if (0) {\n        char nm0"),
        ("M-B the filter admits unnamed ids but keeps the call", K_A, "if (!gb_item_label(g_item_gen, (uint8_t)i, nm0, sizeof nm0)) continue;", "(void)gb_item_label(g_item_gen, (uint8_t)i, nm0, sizeof nm0);"),
        ("M-D the NO ITEM row dropped from held mode", K_D1, "(i == 0 && g_item_allow_none)", "(i == 0 && 0)"),
        ("M-D2 NO ITEM admitted in ADD mode", K_D2, "(i == 0 && g_item_allow_none)", "(i == 0)"),
        ("M-E held row never inserted", K_E, "if (!(g_item_max_id && g_item_allow_none && current != 0", "if (1 || !(g_item_max_id && g_item_allow_none && current != 0"),
        ("M-E2 held row inserted in ADD mode too", K_E1, "g_item_max_id && g_item_allow_none && current != 0", "g_item_max_id && current != 0"),
        ("M-E3 held row appended at the end (not id order)", K_G, "while (at < n && idx[at] < current) at++;", "at = n;"),
        ("M-G2 append-at-end via a never-true scan", K_G, "while (at < n && idx[at] < current) at++;", "while (at < n && idx[at] != current) at++;"),
        ("M-E4 sel not moved to the new row", K_E, "  *np = n + 1;\n  return at;", "  *np = n + 1;\n  return sel;"),
        ("M-C Crystal game ignored (always Gold pocket map)", K_C, "gbb_pocket_of(g_item_game, (uint8_t)i) == GBB_POCKET_COUNT", "gbb_pocket_of(GBF_G_GS, (uint8_t)i) == GBB_POCKET_COUNT"),
        ("M-F named id 0x50 dropped from every list", K_F, "      if (cat) {\n        GbBagPocket want", "      if (i == 0x50) continue;\n      if (cat) {\n        GbBagPocket want"),
    ]
    print("mutants (each must turn its verdict RED):")
    for tag, key, old, new in muts:
        if old not in pick_c:
            print(f"  FAIL {tag}: target not found verbatim")
            bad.append(tag)
            continue
        r = verdicts(run(pick_c.replace(old, new, 1 if not tag.startswith('M-D') else 99)))
        red = [k for k, ok in r.items() if not ok]
        ok = key in red
        print(f"  {'ok  ' if ok else 'FAIL'} {tag}: red = {[k.split()[0] for k in red]}")
        if not ok:
            bad.append(tag)
    if bad:
        print("FAILED:", bad)
        return 1
    print("host_zt_item_list_test: all verdicts hold, every mutant red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
