#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_stack_edges_hygiene_test.py -- pins BACKLOG #204: tools/stack_edges.txt must
carry no "section-anchor" line for source/pdna_pick.c.

Before #204, pdna_pick.c's dex-dispatch family (s_dget/s_dset/s_getnat/s_setnat, four
separate file-static function pointers) rode GCC's -fsection-anchors codegen: every
UNRELATED static pdna_pick.c gained ahead of them shifted their offsets and needed a
fresh "coincidental spilled section-anchor" stack_edges.txt line naming pdna_pick.c
(b195 added two just for one new static and one new parameter -- see that file's own
git history). #204 packed the four pointers into one struct with its own linker
section AND routed every dispatch through four noinline wrapper functions
(dex_dget/dex_dset/dex_getnat_live/dex_setnat_call), so pdna_pick.c needs zero
stack_edges.txt lines of this class ever again.

This is a plain grep, not a build -- catches the regression the instant someone adds
a line back, without needing devkitARM on PATH. Run directly:

    python3 tests/host_stack_edges_hygiene_test.py

Registered in tests/run_host_tests.py's PY_TESTS list.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EDGES = ROOT / "tools" / "stack_edges.txt"

# A LIVE (non-comment) declaration line is `Struct.field @OFF -> impls...` or
# `caller argsites=N -> impls...`. #204's fix means NONE of these may ever again
# name an s_dex field or pdna_dex_screen -- every dispatch the dex-screen family
# needs is covered by the four wrapper functions' own plain argsites=1 lines, which
# no longer mention pdna_dex_screen or s_dex at all. Only LIVE lines are checked
# (comments are free-form prose -- this file's own history section above
# legitimately narrates the section-anchor collision #204 fixed, in the past
# tense; grepping comments for the word would false-positive on that narration).
DECL_RE = re.compile(r'^\s*\S+.*->')


def check_no_pdna_pick_section_anchor_lines() -> list[str]:
    """Returns the list of offending raw lines (empty = clean)."""
    text = EDGES.read_text()
    offenders = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if not DECL_RE.match(line):
            continue
        # A live declaration line (Struct.field @OFF -> ... / caller argsites=N ->
        # ...) is NEVER allowed to name pdna_pick.c's own s_dex/pdna_dex_screen
        # symbols by definition of the #204 fix.
        if 's_dex.' in line or re.search(r'\bpdna_dex_screen\b', line):
            offenders.append(f"{lineno}: {raw}")
    return offenders


def main() -> int:
    failures = []

    offenders = check_no_pdna_pick_section_anchor_lines()
    if offenders:
        failures.append(
            "check_no_pdna_pick_section_anchor_lines: found "
            f"{len(offenders)} offending line(s):\n  " + "\n  ".join(offenders))

    # Sanity: the four wrapper functions ARE declared (argsites=1 each) -- proves
    # the test isn't vacuously passing because the whole block got deleted by
    # accident rather than legitimately fixed.
    text = EDGES.read_text()
    for fn in ("dex_dget", "dex_dset", "dex_getnat_live", "dex_setnat_call"):
        if not re.search(rf'^{fn}\s+argsites=1\s+->', text, re.MULTILINE):
            failures.append(
                f"expected a '{fn} argsites=1 -> ...' declaration line -- "
                "not found (the #204 wrapper declarations look deleted, not fixed)")

    if failures:
        print("FAIL host_stack_edges_hygiene_test.py:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS host_stack_edges_hygiene_test.py (2 checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
