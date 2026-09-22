#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_vsd_funnel_sites_test.py -- BACKLOG #179 A3 review D2: the single-funnel guard.

tools/gb_shots.py's Session.run() is documented as "the single funnel for every frame
in both gb_shots.py and dgb_shots.py" (S4.5) -- the ONE place that calls
`self.core.run_frame()` and, immediately after, services any attached VSD request. Two
long-poll loops in tools/dgb_shots.py (`_measure_box_grid_cold_start()`,
`_measure_b185_auto()`) called `s.core.run_frame()` directly instead, bypassing that
service call for up to 40,000 frames: a pending VSD request timed out mid-poll (S4.3's
16-frame bound), the harness's stale reply then landed in an abandoned buffer, and the
card degraded to NO_FLASHCART for the rest of the run with no error on screen.

This is deliberately dumb (a text grep, in the same "structural check, not a smart
parser" spirit as tests/host_escape_gate_sites_test.py's own header comment): the exact
kind of refactor that silently reintroduces a bare `core.run_frame()` call is exactly
the kind a cleverer AST-based checker could itself be fooled by (an alias, a wrapper
function). Pure Python, stdlib only, never `import mgba` (RUN_HOST_TESTS convention,
tests/run_host_tests.py's own header comment).

Check: every `\\.core\\.run_frame\\(` occurrence in tools/*.py lives inside
tools/gb_shots.py, and inside gb_shots.py ONLY inside Session.run() or
Session._io_quiesce() (the two funnel-internal call sites; both call self.vsd.service()
either directly after or in the surrounding loop). Any other file, or any other
function in gb_shots.py, that calls `.core.run_frame(` directly is the exact regression
this file exists to catch.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"

RUN_FRAME_RE = re.compile(r"\.core\.run_frame\(")
# The two funnel-internal methods in tools/gb_shots.py that are ALLOWED to call
# `self.core.run_frame(` directly -- everything else, in every tools/*.py file
# (including gb_shots.py itself, outside these two methods), is a bypass.
ALLOWED_DEFS = ("def run(self, n: int) -> None:", "def _io_quiesce(self) -> None:")

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def gb_shots_allowed_line_ranges() -> list[tuple[int, int]]:
    """[start, end) 0-based line-index ranges of run()'s and _io_quiesce()'s bodies in
    tools/gb_shots.py, found the same brace/indent-free way this project's other site
    tests extract a Python method body: from its `def` line to the next line at the
    SAME OR LESSER indentation (the next sibling method or a dedent out of the class)."""
    lines = (TOOLS / "gb_shots.py").read_text(encoding="utf-8").split("\n")
    ranges = []
    for sig in ALLOWED_DEFS:
        start = next((i for i, ln in enumerate(lines) if sig in ln), None)
        if start is None:
            raise AssertionError(f"gb_shots.py: signature not found: {sig!r}")
        indent = len(lines[start]) - len(lines[start].lstrip())
        end = len(lines)
        for i in range(start + 1, len(lines)):
            stripped = lines[i].strip()
            if not stripped:
                continue
            cur_indent = len(lines[i]) - len(lines[i].lstrip())
            if cur_indent <= indent:
                end = i
                break
        ranges.append((start, end))
    return ranges


def main() -> int:
    allowed_ranges = gb_shots_allowed_line_ranges()
    gb_shots_lines = (TOOLS / "gb_shots.py").read_text(encoding="utf-8").split("\n")

    violations: list[str] = []
    for py in sorted(TOOLS.glob("*.py")):
        lines = py.read_text(encoding="utf-8").split("\n")
        for i, ln in enumerate(lines):
            if not RUN_FRAME_RE.search(ln):
                continue
            if py.name != "gb_shots.py":
                violations.append(f"{py.name}:{i + 1}: {ln.strip()!r} -- outside "
                                   f"gb_shots.py entirely, not funneled through "
                                   f"Session.run()/_io_quiesce()")
                continue
            inside_allowed = any(start <= i < end for start, end in allowed_ranges)
            if not inside_allowed:
                violations.append(f"gb_shots.py:{i + 1}: {ln.strip()!r} -- outside "
                                   f"Session.run()/Session._io_quiesce()")

    check(not violations,
          "bare .core.run_frame( site(s) found outside the funnel: " + "; ".join(violations))
    # Sanity: the two allowed sites must actually exist and each call self.core.run_frame(
    # at least once, or this check would trivially pass on a gutted gb_shots.py.
    for start, end in allowed_ranges:
        body = gb_shots_lines[start:end]
        check(any(RUN_FRAME_RE.search(ln) for ln in body),
              f"gb_shots.py lines {start + 1}-{end}: expected .core.run_frame( inside "
              f"this funnel method but found none -- the allow-list itself is stale")

    print(f"host_vsd_funnel_sites_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
