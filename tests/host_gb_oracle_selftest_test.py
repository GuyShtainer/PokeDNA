#!/usr/bin/env python3
"""tests/run_host_tests.py's PY_TESTS entries are run with NO extra argv
(subprocess.run([sys.executable, pysrc])), so tools/gb_oracle/celldiff.py's
own `--selftest` flag can't be handed to it directly -- this is the thin,
argv-free wrapper that does. tools/gb_oracle/celldiff.py --selftest itself
is the dead-stub guard (BACKLOG #97): it injects one known-bad cell into two
synthetic grids and demands diff_grids() report EXACTLY that cell, plus a
negative half (two identical grids report zero mismatches) -- so a future
regression back to "a diff loop that never appends a mismatch" fails this
test instead of shipping quietly, the way an earlier ad-hoc GB-screen
reviewer comparator once did.

Needs no ROM, no save, no mGBA -- runs anywhere tools/gb_oracle/celldiff.py
does (its --demo mode is the one that needs the real corpus + mGBA, and is
deliberately NOT registered here for that reason).
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "gb_oracle" / "celldiff.py"),
                         "--selftest"], capture_output=True, text=True)
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="", file=sys.stderr)
    if r.returncode != 0 or "PASS" not in r.stdout:
        print("FAILED: tools/gb_oracle/celldiff.py --selftest did not report PASS")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
