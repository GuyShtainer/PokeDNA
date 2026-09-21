#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_move_pick_test.py -- structural guard for BACKLOG #189 (generation-filtered
move picker).

source/pdna_pick.c does not compile on the host (tonc/libgba UI drawing calls pull in
headers the pure-C host tests deliberately avoid), same reason
tests/host_escape_gate_sites_test.py and tests/host_gb_write_gate_test.py are
text-level checks instead of a compiled unit test. Three things must all hold:

  (a) pick_move_set_gen_max(uint16_t max_id) exists (declared in pdna_pick.h,
      defined in pdna_pick.c) and sets a file-scope static the way
      pick_item_set_gen1_2_max()/g_item_max_id already do.
  (b) build_moves() consults that static -- a ceiling that is never read is a
      setter with no effect.
  (c) EVERY pick_move( call site in source/*.c other than pick_move's own
      definition/declaration is bracketed by a pick_move_set_gen_max( call
      immediately before it and a pick_move_set_gen_max(0) immediately after --
      except pdna_edit.c's Gen-3 editor call, which must stay UNBRACKETED (ceiling
      0 is every existing caller's default; the brief's own constraint: "keep the
      Gen-3 picker behaviour unchanged when the ceiling is 0").

Run directly:

    python3 tests/host_gb_move_pick_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #189).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PICK_H = ROOT / "source" / "pdna_pick.h"
PICK_C = ROOT / "source" / "pdna_pick.c"
GBEDIT_C = ROOT / "source" / "pdna_gbedit.c"
GBSUMMARY_C = ROOT / "source" / "pdna_gbsummary.c"
EDIT_C = ROOT / "source" / "pdna_edit.c"

CALL_RE = re.compile(r"\bpick_move\s*\(")
DEF_RE = re.compile(r"\buint16_t\s+pick_move\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def check_setter_exists(pick_h: str, pick_c: str) -> list[str]:
    v = []
    if not re.search(r"void\s+pick_move_set_gen_max\s*\(\s*uint16_t\s+max_id\s*\)\s*;", pick_h):
        v.append("pdna_pick.h: no `void pick_move_set_gen_max(uint16_t max_id);` declaration")
    if not re.search(r"void\s+pick_move_set_gen_max\s*\(\s*uint16_t\s+max_id\s*\)\s*\{\s*g_move_max_id\s*=\s*max_id\s*;\s*\}",
                     pick_c):
        v.append("pdna_pick.c: pick_move_set_gen_max() is not `{ g_move_max_id = max_id; }` "
                 "over a file-scope static (the pick_item_set_gen1_2_max()/g_item_max_id shape)")
    return v


def check_build_moves_consults(pick_c: str) -> list[str]:
    m = re.search(r"static void build_moves\([^)]*\)\s*\{", pick_c)
    if not m:
        return ["pdna_pick.c: build_moves() not found"]
    start = m.end() - 1
    depth = 1
    i = start + 1
    while i < len(pick_c) and depth > 0:
        if pick_c[i] == "{":
            depth += 1
        elif pick_c[i] == "}":
            depth -= 1
        i += 1
    body = pick_c[start:i]
    if "g_move_max_id" not in body:
        return ["pdna_pick.c: build_moves() never reads g_move_max_id -- the ceiling "
                "setter has no effect on the list"]
    return []


def find_call_lines(path: Path, text: str) -> list[int]:
    """Line numbers of every pick_move( CALL (not the definition, not a comment)."""
    code = strip_comments(text)
    lines = code.splitlines()
    out = []
    for i, line in enumerate(lines, 1):
        if DEF_RE.search(line):
            continue
        if CALL_RE.search(line):
            out.append(i)
    return out


def check_bracketing(path: Path, text: str, want_bracketed: bool) -> list[str]:
    code = strip_comments(text)
    lines = code.splitlines()
    violations = []
    for lineno in find_call_lines(path, text):
        before = lines[lineno - 2] if lineno >= 2 else ""
        after = lines[lineno] if lineno < len(lines) else ""
        has_set = "pick_move_set_gen_max(" in before
        has_clear = "pick_move_set_gen_max(0)" in after
        if want_bracketed:
            if not (has_set and has_clear):
                violations.append(f"{path.name}:{lineno}: pick_move( call is not bracketed by "
                                   f"pick_move_set_gen_max(...)/pick_move_set_gen_max(0) on the "
                                   f"immediately surrounding lines")
        else:
            if has_set or has_clear:
                violations.append(f"{path.name}:{lineno}: the Gen-3 editor's pick_move( call is "
                                   f"unexpectedly bracketed -- ceiling 0 must stay every existing "
                                   f"caller's default (constraint: unchanged Gen-3 behaviour)")
    return violations


def run_all() -> list[str]:
    pick_h = PICK_H.read_text()
    pick_c = PICK_C.read_text()
    gbedit_c = GBEDIT_C.read_text()
    gbsummary_c = GBSUMMARY_C.read_text()
    edit_c = EDIT_C.read_text()

    v = []
    v += check_setter_exists(pick_h, pick_c)
    v += check_build_moves_consults(pick_c)
    v += check_bracketing(GBEDIT_C, gbedit_c, want_bracketed=True)
    # BACKLOG #189 brief note: grep pdna_gbsummary.c for a second pick_move( call site --
    # none exists today (gbsum_edit_keys routes moves through gbedit_press, already
    # covered by the pdna_gbedit.c check above), but check it anyway so a future direct
    # call there does not silently ship unbracketed.
    v += check_bracketing(GBSUMMARY_C, gbsummary_c, want_bracketed=True)
    v += check_bracketing(EDIT_C, edit_c, want_bracketed=False)
    return v


def main() -> int:
    if not (PICK_H.exists() and PICK_C.exists() and GBEDIT_C.exists() and EDIT_C.exists()):
        print("SKIP (source files not found)")
        return 0

    violations = run_all()
    if violations:
        print("FAIL -- BACKLOG #189 move-picker ceiling gap:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: pick_move_set_gen_max exists, build_moves() consults it, every GB editor "
          "pick_move( call site is bracketed, the Gen-3 editor's call site is not")

    # --- self-mutation 1: drop the g_move_max_id consult from build_moves() ----------
    import tempfile
    tmpdir = Path(tempfile.mkdtemp(prefix="gbmovepick_"))
    try:
        pick_c = PICK_C.read_text()
        target = "    if (g_move_max_id && m > g_move_max_id) continue;   /* BACKLOG #189: the gen ceiling */\n"
        if target not in pick_c:
            print(f"FAIL -- self-mutation 1 target line not found verbatim: {target!r} "
                  f"(source drifted -- update this test)")
            return 1
        scratch_pick_c = tmpdir / "pdna_pick.c"
        scratch_pick_c.write_text(pick_c.replace(target, "", 1))
        m = re.search(r"static void build_moves\([^)]*\)\s*\{", scratch_pick_c.read_text())
        body_violations = check_build_moves_consults(scratch_pick_c.read_text())
        if not body_violations:
            print("FAIL -- self-mutation 1: deleting the g_move_max_id consult in "
                  "build_moves() did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation 1: deleting the g_move_max_id consult -- correctly caught:")
        for bv in body_violations:
            print(f"  (mutated-copy) FAIL: {bv}")

        # --- self-mutation 2: drop the bracket around pdna_gbedit.c's pick_move( call --
        gbedit_c = GBEDIT_C.read_text()
        target2 = "    pick_move_set_gen_max(gb_max_move(e->gen));\n"
        if target2 not in gbedit_c:
            print(f"FAIL -- self-mutation 2 target line not found verbatim: {target2!r} "
                  f"(source drifted -- update this test)")
            return 1
        scratch_gbedit_c = tmpdir / "pdna_gbedit.c"
        scratch_gbedit_c.write_text(gbedit_c.replace(target2, "", 1))
        bracket_violations = check_bracketing(GBEDIT_C, scratch_gbedit_c.read_text(), want_bracketed=True)
        if not bracket_violations:
            print("FAIL -- self-mutation 2: dropping the pick_move_set_gen_max( bracket "
                  "did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation 2: dropping pdna_gbedit.c's pick_move_set_gen_max bracket -- "
              "correctly caught:")
        for bv in bracket_violations:
            print(f"  (mutated-copy) FAIL: {bv}")
    finally:
        import shutil
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nhost_gb_move_pick_test: ok (shipped source clean, both mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
