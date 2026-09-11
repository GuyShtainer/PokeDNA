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


def main() -> int:
    os.chdir(ROOT)
    srcs = sorted(glob.glob("tests/host_*_test.c"))

    saves = sorted(glob.glob(str(ROMS / "*.sav"))) or \
        sorted(glob.glob(str(FIXTURES / "*.sav")))

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
            args = saves if re.search(r"argv\[\w+\]", src.read_text(errors="replace")) else []
            r = subprocess.run([binpath, *args], capture_output=True, text=True)
            out = (r.stdout + r.stderr).strip()

            skip_line = next((ln for ln in out.splitlines() if ln.startswith("SKIP (")), None)
            if r.returncode == 0 and skip_line:
                print(f"  {name:<26} {skip_line}")   # a test that found a fixture absent, on ANY line (BACKLOG #115)
                nskip += 1
            elif r.returncode == 0:
                print(f"  {name:<26} ok")
                npass += 1
            else:
                print(f"  {name:<26} FAILED (exit {r.returncode})")
                for ln in [l for l in out.split("\n") if "fail" in l.lower()][:8]:
                    print(f"      {ln}")
                nfail += 1
                failed.append(name)
            if VERBOSE and out:
                for ln in out.split("\n"):
                    print(f"      {ln}")
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
