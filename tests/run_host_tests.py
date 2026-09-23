#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""run_host_tests.py — build and run EVERY pure-C host test, and give one verdict.

Each tests/host_*_test.c documents its own `cc` line in its header comment, because the
object list differs per test and getting it wrong is a link error rather than a silent
skip. This script extracts that line, runs it, and tallies the result — so "did I break
anything" is one command instead of twenty-eight copy-pastes.

    python3 tests/run_host_tests.py          # from the project root
    python3 tests/run_host_tests.py -v       # also echo each test's stdout

Tests that take a .sav argument are given the local 5-game corpus when it is present,
else tests/fixtures/*.sav. Those saves are Guy's own cartridge dumps: they live OUTSIDE
the repo, are gitignored, and are never published.

Exit status is 0 only if every test that could be built also passed.

NOT covered here: tools/gb_retail_gate.py (`make retail-gate`) boots EDITED Gen-1/2
saves in the real ROM under mGBA and asserts on the screen the game drew. It is
deliberately excluded from this default loop -- ~30 emulator boots vs. the near-instant
pure-C tests below -- but it is the gate anything touching gb_session/gb_edit/gb_editor
should also pass before being called done.
"""
from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROMS = Path(os.environ.get(
    "ROMS", "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"))
FIXTURES = ROOT / "tests" / "fixtures"

# BACKLOG #130: host_stack_budget_test.py is a pure-Python unit test of a pure-Python
# tool (tools/stack_budget.py) — nothing to compile, so it doesn't fit the `cc -std=c11`
# extraction below. It used to be reachable only via `make stack-check`, which nothing
# else runs by default, so a real regression (the BoxSource offset drift, BACKLOG #130)
# sat failing on main unnoticed. Named explicitly rather than globbed: tests/host_fusegb_test.py
# is also pure-Python and deliberately stays off this list (own ticket, out of scope here).
PY_TESTS = ["tests/host_stack_budget_test.py", "tests/host_gb_write_gate_test.py", "tests/host_xfer_rekey_order_test.py",
            "tests/host_itemmap_gen_test.py", "tests/host_escape_gate_sites_test.py",
            "tests/host_gb_oracle_selftest_test.py", "tests/host_gb_origin_key_test.py",
            "tests/host_lift_why_gate_test.py", "tests/host_first_lift_repage_test.py",
            "tests/host_gb_carry_session_exit_test.py",
            "tests/host_gb_move_pick_test.py", "tests/host_box_resume_test.py",
            "tests/host_browser_parity_test.py",
            "tests/host_gb_grid_ops_test.py",
            "tests/host_gb_item_pick_test.py",
            "tests/host_gb_grid_blocked_test.py",
            "tests/host_stack_edges_hygiene_test.py",
            "tests/host_xfer_view_readonly_test.py",
            "tests/host_gb_claims_test.py",
            "tests/host_xfer_reconcile_sites_test.py",
            "tests/host_bank_meta_backup_sites_test.py",
            "tests/host_vsd_funnel_sites_test.py",
            "tests/host_clip_session_clear_test.py",
            "tests/host_partymail_gate_test.py"]

VERBOSE = "-v" in sys.argv


def cc_line_for(src: Path) -> str | None:
    """Pull the `cc -std=c11 ... -o /tmp/x` line out of the file's header comment.

    The line usually wraps over 2-3 lines with trailing backslashes and a leading ` * `
    on the continuations. Everything from `&&` onwards is the author running the binary,
    which we do ourselves.
    """
    head = src.read_text(errors="replace").split("\n")[:24]
    buf, collecting = [], False
    for line in head:
        stripped = re.sub(r"^\s*\*?\s?", "", line)
        if not collecting and "cc -std=c11" in stripped:
            collecting = True
            stripped = stripped[stripped.index("cc -std=c11"):]
        elif not collecting:
            continue
        cont = stripped.rstrip().endswith("\\")
        buf.append(stripped.rstrip().rstrip("\\").strip())
        if not cont:
            break
    if not buf:
        return None
    cmd = " ".join(buf)
    cmd = cmd.split("&&")[0].strip()
    return cmd or None


def classify(r: subprocess.CompletedProcess) -> tuple[str, str]:
    """Shared verdict for a finished test process (compiled binary or `python3 <file>`):
    ("ok"|"skip"|"fail", detail-to-print). A SKIP line anywhere in stdout/stderr wins
    over a zero exit (BACKLOG #115 — a test that found a fixture absent still exits 0)."""
    out = (r.stdout + r.stderr).strip()
    skip_line = next((ln for ln in out.splitlines() if ln.startswith("SKIP (")), None)
    if r.returncode == 0 and skip_line:
        return "skip", skip_line
    if r.returncode == 0:
        return "ok", out
    fail_lines = [ln for ln in out.split("\n") if "fail" in ln.lower()][:8]
    header = f"FAILED (exit {r.returncode})"
    return "fail", "\n".join([header] + [f"      {ln}" for ln in fail_lines])


def tally(name, outcome, detail, npass, nfail, nskip, failed):
    if outcome == "skip":
        print(f"  {name:<26} {detail}")
        nskip += 1
    elif outcome == "ok":
        print(f"  {name:<26} ok")
        npass += 1
        if VERBOSE and detail:
            for ln in detail.split("\n"):
                print(f"      {ln}")
    else:
        lines = detail.split("\n")
        print(f"  {name:<26} {lines[0]}")
        for ln in lines[1:]:
            print(ln)
        nfail += 1
        failed.append(name)
    return npass, nfail, nskip


def main() -> int:
    os.chdir(ROOT)
    srcs = sorted(glob.glob("tests/host_*_test.c"))

    saves = sorted(glob.glob(str(ROMS / "*.sav"))) or \
        sorted(glob.glob(str(FIXTURES / "*.sav")))
    # BACKLOG #140: a test can opt OUT of the default "argv means .sav corpus"
    # convention by carrying this exact marker line in its header comment -- it
    # gets the .gba corpus instead (host_render_test.c wants a ROM, not a save).
    roms = sorted(glob.glob(str(ROMS / "*.gba")))

    npass = nfail = nskip = 0
    failed: list[str] = []

    tmpdir = tempfile.mkdtemp(prefix="pdna_host-")
    try:
        for s in srcs:
            src = Path(s)
            name = src.stem
            cmd = cc_line_for(src)
            if not cmd:
                print(f"  {name:<26} SKIP (no cc line in its header)")
                nskip += 1
                continue

            binpath = os.path.join(tmpdir, f"pdna_{name}")
            cmd = re.sub(r"-o\s+\S+", f"-o {binpath}", cmd)
            if f"-o {binpath}" not in cmd:
                cmd += f" -o {binpath}"

            b = subprocess.run(cmd, shell=True, capture_output=True, text=True)
            if b.returncode != 0:
                print(f"  {name:<26} BUILD FAILED")
                for ln in b.stderr.strip().split("\n")[:6]:
                    print(f"      {ln}")
                nfail += 1
                failed.append(name + " (build)")
                continue

            # Any indexing of argv means "this test takes saves". It used to look for the
            # literal `argv[1]`, which host_legality_hooks_test.c does not contain (it loops
            # `argv[i]`), so that test ran with an EMPTY corpus and failed four checks whose
            # whole point is that the corpus is non-empty — a red line in every run that had
            # nothing to do with the code under test.
            #
            # BACKLOG #140: the `RUN_HOST_TESTS: WANTS_ROM_ARGV` marker line overrides that
            # default -- the test wants the .gba corpus instead (a ROM, not a save). Checked
            # BEFORE the generic argv[] sniff below since a ROM-wanting test still indexes
            # argv[1] and would otherwise match the .sav branch.
            text = src.read_text(errors="replace")
            if "RUN_HOST_TESTS: WANTS_ROM_ARGV" in text:
                args = roms
            elif re.search(r"argv\[\w+\]", text):
                args = saves
            else:
                args = []
            r = subprocess.run([binpath, *args], capture_output=True, text=True)
            outcome, detail = classify(r)
            npass, nfail, nskip = tally(name, outcome, detail, npass, nfail, nskip, failed)

        for pysrc in PY_TESTS:
            name = Path(pysrc).stem
            r = subprocess.run([sys.executable, pysrc], capture_output=True, text=True)
            outcome, detail = classify(r)
            npass, nfail, nskip = tally(name, outcome, detail, npass, nfail, nskip, failed)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    print(f"host tests: {npass} passed, {nfail} failed, {nskip} skipped "
          f"({len(saves)} save(s) supplied to tests that take one)")
    if failed:
        print("failing:", ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
