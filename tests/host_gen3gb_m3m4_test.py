#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_gen3gb_m3m4_test.py -- F6 (xfer-items fix pass review): mutations M3/M4, made real.

tests/host_gen3gb_test.c PRINTED two "mutation proof" claims (M3: comment out set_item's
`if (gen == GB_GEN2)` mapped branch; M4: change exp_floored's `>` to `>=` in
set_identity_and_level, both source/gen3_to_gb.c) instead of running them -- a printf
inside a test binary is not evidence (F6's own words). Both mutations are ordinary
one-line source patches to a real, linked file, so unlike M5 (which needed a synthetic
name table) they need no fixture at all: build the WHOLE existing
tests/host_gen3gb_test.c against a scratch-mutated copy of source/gen3_to_gb.c, using
its own compile recipe (its header comment), and confirm the specific named check the
mutant should break goes RED while the suite as a whole still links and runs -- proof
the checks the report already named ("8a: held item == 1" / "8e: exactly-at-floor exp
-> exp_floored is false") are load-bearing, not just present.

Both mutant binaries also need Guy's own corpus (.sav files passed as argv, same as the
real test) ONLY for their non-mutated sections; section 8's own checks run corpus-free
(gen3_build_mon() synthesizes the record), so this test never touches the corpus at all
-- it only needs the mutant binary to LINK and RUN, and greps its stdout for the one
named failure.

Pure Python, stdlib only, never `import mgba` (tests/run_host_tests.py's own
RUN_HOST_TESTS convention). Host-only: builds two mutant binaries in a temp dir, runs
them, deletes them.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "source"
TESTS = ROOT / "tests"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


# M3: set_item()'s Gen-2 mapped-item branch disabled -- a held item that would map
# (item_g3_to_g2 != 0) now falls through to the STAYS/dropped tail instead of HELD.
M3_FROM = "  if (gen == GB_GEN2) {\n    uint8_t mapped = item_g3_to_g2(m->heldItem);"
M3_TO = "  if (0 && gen == GB_GEN2) {   /* MUT M3: mapped branch disabled */\n    uint8_t mapped = item_g3_to_g2(m->heldItem);"

# M4: exp_floored's strict '>' relaxed to '>=' -- a record sitting EXACTLY at its
# level's exp floor (no progress lost) now wrongly reports a loss.
M4_FROM = "  loss->exp_floored = m->experience > pk_exp_for_level(g3_growth, m->level);"
M4_TO = "  loss->exp_floored = m->experience >= pk_exp_for_level(g3_growth, m->level);   /* MUT M4 */"

GEN3_TO_GB_SRCS = [
    "item_map_g2g3.c", "item_map_g1g2.c", "gb_item_names.c", "gb_bag.c", "gb_fields.c",
    # gen3_to_gb.c substituted per-build below
    "gb_sidecar.c", "bank_cell.c", "evolutions.c",
    "gen3_save.c", "gen3_mon.c", "gen3_box.c", "gen3_edit.c",
    "gen3_daycare.c", "data_tables.c",
    "gb_edit.c", "gb_session.c", "gen1_save.c", "gen1_write.c",
    "gen2_save.c", "gen2_write.c", "gb_moves_legal.c",
]


def build_and_run(mutant_impl: Path, out_bin: Path) -> tuple[int, str]:
    cmd = (["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(SOURCE),
           str(TESTS / "host_gen3gb_test.c"), str(mutant_impl)] +
          [str(SOURCE / s) for s in GEN3_TO_GB_SRCS] +
          ["-o", str(out_bin)])
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        return -1, (proc.stdout + proc.stderr)
    run = subprocess.run([str(out_bin)], capture_output=True, text=True)
    return run.returncode, run.stdout + run.stderr


def main() -> int:
    real_src = (SOURCE / "gen3_to_gb.c").read_text(encoding="utf-8")
    check(M3_FROM in real_src,
          "gen3_to_gb.c: M3's target text has drifted -- update M3_FROM in this test")
    check(M4_FROM in real_src,
          "gen3_to_gb.c: M4's target text has drifted -- update M4_FROM in this test")
    if M3_FROM not in real_src or M4_FROM not in real_src:
        print(f"host_gen3gb_m3m4_test: {checks} checks, {len(fails)} failed")
        for f in fails:
            print(f"  FAIL: {f}")
        return 1

    with tempfile.TemporaryDirectory(prefix="host_gen3gb_m3m4_") as td:
        work = Path(td)

        m3_src = real_src.replace(M3_FROM, M3_TO)
        check(m3_src != real_src, "M3 patch did not change the source (no-op replace)")
        m3_impl = work / "gen3_to_gb_m3.c"
        m3_impl.write_text(m3_src, encoding="utf-8")
        rc3, out3 = build_and_run(m3_impl, work / "hg3gb_m3")
        check(rc3 == -1 or rc3 != 0,
              f"MUT M3: expected the suite to fail to build or exit non-zero -- got rc={rc3}")
        check("8a: held item == 1" in out3,
              f"MUT M3: expected the named failure '8a: held item == 1' in stdout -- "
              f"not found (rc={rc3}); tail: {out3[-600:]!r}")

        m4_src = real_src.replace(M4_FROM, M4_TO)
        check(m4_src != real_src, "M4 patch did not change the source (no-op replace)")
        m4_impl = work / "gen3_to_gb_m4.c"
        m4_impl.write_text(m4_src, encoding="utf-8")
        rc4, out4 = build_and_run(m4_impl, work / "hg3gb_m4")
        check(rc4 == -1 or rc4 != 0,
              f"MUT M4: expected the suite to fail to build or exit non-zero -- got rc={rc4}")
        check("8e: exactly-at-floor exp -> exp_floored is false" in out4,
              f"MUT M4: expected the named failure '8e: exactly-at-floor exp -> "
              f"exp_floored is false' in stdout -- not found (rc={rc4}); "
              f"tail: {out4[-600:]!r}")

    print(f"host_gen3gb_m3m4_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
