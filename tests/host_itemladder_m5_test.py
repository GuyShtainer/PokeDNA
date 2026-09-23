#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_itemladder_m5_test.py -- F6 (xfer-items fix pass review): mutation M5, made real.

tests/host_itemladder_test.c used to PRINT an eleven-line claim about mutation M5 (the
"or vice versa" reverse-ambiguity check inside item_g2_to_g1(), source/item_map_g1g2.c)
instead of running it: "verified in the report, not reproduced by this binary". Deleting
the g2_count block there left the suite GREEN, because no REAL Gen-1/Gen-2 name pair in
today's tables actually triggers reverse ambiguity -- the claim needed a SYNTHETIC name
table to mean anything, which is exactly what a printf cannot build.

This builds that synthetic table for real: a scratch copy of source/gb_item_names.c with
Gen-2 id 0x03 ("BRIGHTPOWDER") relabelled "MASTER BALL" -- aliasing Gen-2 id 0x01, which
already carries that name. item_g2_to_g1(1) against this aliased table must find g1_match
== 1 (Gen-1's own "MASTER BALL", also id 0x01, not a key item) with g1_count == 1 (no
OTHER Gen-1 id shares the name), then refuse anyway because g2_count == 2 (ids 0x01 AND
0x03 both spell "MASTER BALL" now) -- the exact "or vice versa" case the check exists
for. Two real builds, no synthetic C source needed for the DECISION logic itself (only
the name table is faked):
  - REAL item_map_g1g2.c + the aliased table -> item_g2_to_g1(1) must be 0 (refused).
  - MUTANT item_map_g1g2.c (the g2_count block deleted, source-level patch below)
    + the SAME aliased table -> item_g2_to_g1(1) must be 1 (g1_match, the wrong answer)
    -- proof the real check is load-bearing, not that the aliased table alone forces 0.

Pure Python, stdlib only, never `import mgba` (tests/run_host_tests.py's own
RUN_HOST_TESTS convention). Host-only: builds two tiny binaries in a temp dir, runs them,
deletes them.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "source"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


DRIVER_C = """
#include <stdio.h>
#include <stdint.h>
#include "item_map_g1g2.h"
int main(void) {
  printf("%u\\n", (unsigned)item_g2_to_g1(1));
  return 0;
}
"""

# BACKLOG #249's own item_map_g1g2.c, with the reverse (g2_count) ambiguity block
# replaced by a no-op that always accepts -- the M5 mutation this file exists to prove
# has teeth. Kept here as a source-level patch (not a compile flag inside the real
# file) so the real file under test/source/ is never touched.
MUTANT_PATCH_FROM = """  /* "or vice versa": the one Gen-1 candidate's name must not ALSO belong to more than
   * one Gen-2 id -- a name shared by two Gen-2 items is not a clean 1:1 mapping either,
   * even though each of those two would individually resolve to the same g1_match. */
  int g2_count = 0;
  for (int g2id = 1; g2id <= 255; g2id++) {
    const char* n = gb2_item_name((uint8_t)g2id);
    if (n && n[0] != '\\0' && strcmp(n, g2name) == 0) g2_count++;
  }
  if (g2_count != 1) return 0;
"""
MUTANT_PATCH_TO = "  /* MUT M5: reverse-ambiguity check deleted -- always accept. */\n"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="host_itemladder_m5_") as td:
        work = Path(td)

        # ---- the synthetic name table: alias Gen-2 id 0x03 to "MASTER BALL" ----
        names_src = (SOURCE / "gb_item_names.c").read_text(encoding="utf-8")
        target = '  "BRIGHTPOWDER", /* 0x03 */\n'
        check(names_src.count(target) == 1,
              f"gb_item_names.c: expected exactly one {target!r} line to alias "
              f"(source drifted -- update this test's target string)")
        if names_src.count(target) != 1:
            print(f"host_itemladder_m5_test: {checks} checks, {len(fails)} failed")
            for f in fails:
                print(f"  FAIL: {f}")
            return 1
        aliased_src = names_src.replace(target, '  "MASTER BALL", /* 0x03, ALIASED for M5 */\n')
        aliased_names = work / "gb_item_names_aliased.c"
        aliased_names.write_text(aliased_src, encoding="utf-8")

        driver = work / "driver.c"
        driver.write_text(DRIVER_C, encoding="utf-8")

        # ---- REAL item_map_g1g2.c + the aliased table ----
        real_bin = work / "real"
        proc = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(SOURCE),
             str(driver), str(SOURCE / "item_map_g1g2.c"), str(aliased_names),
             str(SOURCE / "item_map_g2g3.c"),
             str(SOURCE / "gb_bag.c"), str(SOURCE / "gb_fields.c"),
             str(SOURCE / "gb_session.c"), str(SOURCE / "gen1_save.c"),
             str(SOURCE / "gen1_write.c"), str(SOURCE / "gen2_save.c"),
             str(SOURCE / "gen2_write.c"), str(SOURCE / "gb_edit.c"),
             str(SOURCE / "gen3_to_gb.c"),
             str(SOURCE / "gb_sidecar.c"), str(SOURCE / "bank_cell.c"),
             str(SOURCE / "gen3_save.c"), str(SOURCE / "gen3_mon.c"),
             str(SOURCE / "gen3_box.c"), str(SOURCE / "gen3_edit.c"),
             str(SOURCE / "gen3_daycare.c"), str(SOURCE / "data_tables.c"),
             str(SOURCE / "evolutions.c"), str(SOURCE / "gb_moves_legal.c"),
             "-o", str(real_bin)],
            cwd=ROOT, capture_output=True, text=True)
        check(proc.returncode == 0,
              f"real build failed: {(proc.stdout + proc.stderr).strip()}")
        if proc.returncode == 0:
            out = subprocess.run([str(real_bin)], capture_output=True, text=True).stdout.strip()
            check(out == "0",
                  f"REAL item_map_g1g2.c + aliased table: item_g2_to_g1(1) should refuse "
                  f"(0) on reverse ambiguity -- got {out!r}")

        # ---- MUTANT item_map_g1g2.c (g2_count check deleted) + the SAME aliased table ----
        real_impl_src = (SOURCE / "item_map_g1g2.c").read_text(encoding="utf-8")
        check(MUTANT_PATCH_FROM in real_impl_src,
              "item_map_g1g2.c: the g2_count block text has drifted -- update "
              "MUTANT_PATCH_FROM in this test")
        mutant_src = real_impl_src.replace(MUTANT_PATCH_FROM, MUTANT_PATCH_TO)
        mutant_impl = work / "item_map_g1g2_mutant.c"
        mutant_impl.write_text(mutant_src, encoding="utf-8")

        mutant_bin = work / "mutant"
        proc2 = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(SOURCE),
             str(driver), str(mutant_impl), str(aliased_names),
             str(SOURCE / "item_map_g2g3.c"),
             str(SOURCE / "gb_bag.c"), str(SOURCE / "gb_fields.c"),
             str(SOURCE / "gb_session.c"), str(SOURCE / "gen1_save.c"),
             str(SOURCE / "gen1_write.c"), str(SOURCE / "gen2_save.c"),
             str(SOURCE / "gen2_write.c"), str(SOURCE / "gb_edit.c"),
             str(SOURCE / "gen3_to_gb.c"),
             str(SOURCE / "gb_sidecar.c"), str(SOURCE / "bank_cell.c"),
             str(SOURCE / "gen3_save.c"), str(SOURCE / "gen3_mon.c"),
             str(SOURCE / "gen3_box.c"), str(SOURCE / "gen3_edit.c"),
             str(SOURCE / "gen3_daycare.c"), str(SOURCE / "data_tables.c"),
             str(SOURCE / "evolutions.c"), str(SOURCE / "gb_moves_legal.c"),
             "-o", str(mutant_bin)],
            cwd=ROOT, capture_output=True, text=True)
        check(proc2.returncode == 0,
              f"mutant build failed: {(proc2.stdout + proc2.stderr).strip()}")
        if proc2.returncode == 0:
            out2 = subprocess.run([str(mutant_bin)], capture_output=True, text=True).stdout.strip()
            check(out2 == "1",
                  f"MUTANT item_map_g1g2.c (no reverse check) + aliased table: "
                  f"item_g2_to_g1(1) should wrongly accept (1, the ambiguous g1_match) "
                  f"-- got {out2!r} (if this is '0' too, the mutant has no teeth: the "
                  f"aliased table alone would already force the right answer)")

    print(f"host_itemladder_m5_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
