#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_box_resume_test.py -- structural guard for BACKLOG #188 (pdna_box()'s
resume-cell hint).

source/pdna_box.c does not compile on the host (tonc/OAM/UI calls), same reason
tests/host_gb_write_gate_test.py and tests/host_escape_gate_sites_test.py are
text-level checks instead of a compiled unit test. Four things must all hold:

  (a) app_box_resume_note/app_box_resume_take/app_box_resume_clear are declared in
      pdna_app.h and implemented in pdna_main.c over <= 8 bytes of file-scope state
      (two `int`s -- the brief's own budget).
  (b) EVERY `boxoam_exit(); return N;` site inside pdna_box() (BoxSource* src) is
      immediately preceded by `app_box_resume_note(box, cur);` -- a return that
      forgets to note the cell is exactly the "only the BOX is restored, not the
      CELL" bug BACKLOG #188 reports. Counted, not just checked non-zero: a future
      return site added without the note call must fail this the same way a
      deleted one does.
  (c) pdna_box()'s own entry block applies app_box_resume_take(box) only inside an
      `st == 0` (no directional hint) branch, gated on `pickup_ps < 0` (a day-care
      pickup already placed the cursor) -- the entry-hint contract app_box_start_take()
      already documents must not be overridden by a resume.
  (d) app_box_resume_clear() is called from view_save() (Gen 3's own mount) and from
      all three GBA-side pdna_gen12_mount() call sites (Game Boy) -- never from
      inside pdna_gen12_mount() itself (that function is compiled on the host by
      tests/host_gen12_test.c and must stay free of pdna_app.h).

Run directly:

    python3 tests/host_box_resume_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #188).
"""
import re
import sys
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_H = ROOT / "source" / "pdna_app.h"
MAIN_C = ROOT / "source" / "pdna_main.c"
BOX_C = ROOT / "source" / "pdna_box.c"
GEN12_C = ROOT / "source" / "pdna_gen12.c"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    m = re.search(r"^\w[\w \*]*\b" + re.escape(func_name) + r"\s*\([^;]*?\)\s*\{",
                  text, re.MULTILINE)
    if not m:
        m = re.search(r"\b" + re.escape(func_name) + r"\s*\([^;{]*\)\s*\{", text)
        if not m:
            return ""
    start = m.start()
    i = m.end() - 1
    assert text[i] == "{"
    depth = 1
    j = i + 1
    while j < len(text) and depth > 0:
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
        j += 1
    return text[start:j]


def check_api_declared(app_h: str, main_c: str) -> list[str]:
    v = []
    for sig in (r"void\s+app_box_resume_note\s*\(\s*int\s+box\s*,\s*int\s+cur\s*\)\s*;",
                r"int\s+app_box_resume_take\s*\(\s*int\s+box\s*\)\s*;",
                r"void\s+app_box_resume_clear\s*\(\s*void\s*\)\s*;"):
        if not re.search(sig, app_h):
            v.append(f"pdna_app.h: missing declaration matching /{sig}/")
    if "static int g_resume_box" not in main_c or "g_resume_cell" not in main_c:
        v.append("pdna_main.c: no `static int g_resume_box ... g_resume_cell` "
                 "file-scope state (the <= 8 B budget)")
    for name in ("app_box_resume_note", "app_box_resume_take", "app_box_resume_clear"):
        if f"void {name}(" not in main_c and f"int  {name}(" not in main_c and f"int {name}(" not in main_c:
            v.append(f"pdna_main.c: no definition found for {name}()")
    return v


def find_return_sites(box_c: str) -> list[tuple[int, str]]:
    """Every `boxoam_exit(); return N;` line inside pdna_box(), (lineno, line)."""
    body = extract_function_body(box_c, "pdna_box")
    if not body:
        return []
    code = strip_comments(body)
    out = []
    base_line = box_c[:box_c.index(body)].count("\n") + 1
    for i, line in enumerate(code.splitlines()):
        if "boxoam_exit(); return" in line:
            out.append((base_line + i, line.strip()))
    return out


def check_return_sites_noted(box_c: str) -> list[str]:
    sites = find_return_sites(box_c)
    if not sites:
        return ["pdna_box(): no `boxoam_exit(); return N;` sites found -- function "
                "shape drifted, update this test"]
    violations = []
    for lineno, line in sites:
        if "app_box_resume_note(box, cur); boxoam_exit(); return" not in line:
            violations.append(f"pdna_box.c line ~{lineno}: return site has no "
                              f"app_box_resume_note(box, cur); immediately before "
                              f"boxoam_exit(): {line!r}")
    return violations, len(sites)


def check_entry_gated(box_c: str) -> list[str]:
    body = extract_function_body(box_c, "pdna_box")
    code = strip_comments(body)
    if "app_box_resume_take(box)" not in code:
        return ["pdna_box(): app_box_resume_take(box) is never called"]
    # Must sit inside an `st == 0` branch (no directional hint), not unconditional.
    m = re.search(r"else if \(st == 0[^)]*\)\s*\{[^}]*app_box_resume_take\(box\)", code, re.DOTALL)
    if not m:
        return ["pdna_box(): app_box_resume_take(box) is not gated on `st == 0` -- "
                "it could override a directional entry hint"]
    if "pickup_ps < 0" not in m.group(0):
        return ["pdna_box(): the st == 0 resume branch does not also check "
                "pickup_ps < 0 -- a day-care pickup's own cursor placement could "
                "be silently overwritten"]
    return []


def check_clear_sites(main_c: str, gen12_c: str) -> list[str]:
    v = []
    if "app_box_resume_clear();" not in main_c:
        v.append("pdna_main.c: view_save() never calls app_box_resume_clear()")
    n = gen12_c.count("app_box_resume_clear();")
    if n < 3:
        v.append(f"pdna_gen12.c: only {n} app_box_resume_clear() call site(s) found, "
                 f"expected 3 (one per pdna_gen12_mount() GBA-side caller)")
    mount_body = extract_function_body(gen12_c, "pdna_gen12_mount")
    if "app_box_resume_clear" in strip_comments(mount_body):
        v.append("pdna_gen12_mount(): calls app_box_resume_clear() from INSIDE the "
                 "host-compiled mount function -- pdna_app.h is a GBA-app header, "
                 "this would break tests/host_gen12_test.c's host build")
    return v


def run_all(app_h, main_c, box_c, gen12_c) -> tuple[list[str], int]:
    v = []
    v += check_api_declared(app_h, main_c)
    rs_violations, n_sites = check_return_sites_noted(box_c) if find_return_sites(box_c) else ([], 0)
    v += rs_violations
    v += check_entry_gated(box_c)
    v += check_clear_sites(main_c, gen12_c)
    v += check_take_body_guards(main_c)
    return v, n_sites



def check_take_body_guards(main_c: str) -> list[str]:
    """app_box_resume_take(box) must refuse a recorded cell from a DIFFERENT box
    (brief case (d): a stale cell from box 2 must never apply when box 5 is entered)."""
    m = re.search(r"int\s+app_box_resume_take\(int box\)\s*\{(.*?)\n\}", main_c, re.S)
    if not m:
        return ["app_box_resume_take() not found in source/pdna_main.c"]
    body = m.group(1)
    if "g_resume_box != box" not in body:
        return ["app_box_resume_take() has no box mismatch guard (g_resume_box != box) -- "
                "a stale cell recorded in another box would apply"]
    return []

def main() -> int:
    if not (APP_H.exists() and MAIN_C.exists() and BOX_C.exists() and GEN12_C.exists()):
        print("SKIP (source files not found)")
        return 0

    app_h, main_c, box_c, gen12_c = (p.read_text() for p in (APP_H, MAIN_C, BOX_C, GEN12_C))
    violations, n_sites = run_all(app_h, main_c, box_c, gen12_c)
    print(f"pdna_box() return sites checked: {n_sites}")
    if violations:
        print("FAIL -- BACKLOG #188 resume-cell gap:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: resume API declared over <= 8 B of state, all "
          f"{n_sites} pdna_box() return sites note (box, cur), entry is gated on "
          "st == 0 && pickup_ps < 0, and app_box_resume_clear() is called at all "
          "4 real mount sites (never from inside the host-pure pdna_gen12_mount())")

    # --- self-mutation 1: delete ONE resume_note call from a return site -----------
    tmpdir = Path(tempfile.mkdtemp(prefix="boxresume_"))
    try:
        target = ("else if (src->is_bank && cur + COLS >= COLS * ROWS) { app_box_resume_note(box, cur); boxoam_exit(); "
                  "return 5; }     /* off the PHYSICAL Bank bottom -> PC, still holding; a capacity edge stays put (b200 review A1) */")
        if target not in box_c:
            print(f"FAIL -- self-mutation 1 target line not found verbatim: {target!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated = box_c.replace(
            target,
            target.replace("app_box_resume_note(box, cur); boxoam_exit();", "boxoam_exit();"), 1)
        v1, n1 = run_all(app_h, main_c, mutated, gen12_c)
        if not any("no app_box_resume_note" in x for x in v1):
            print("FAIL -- self-mutation 1: deleting one return site's resume_note "
                  "call did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation 1: deleting one return site's app_box_resume_note( call "
              "-- correctly caught:")
        for x in v1:
            if "no app_box_resume_note" in x:
                print(f"  (mutated-copy) FAIL: {x}")

        # --- self-mutation 2: delete the pickup_ps < 0 guard on the resume branch --
        target2 = "else if (st == 0 && pickup_ps < 0) {"
        if target2 not in box_c:
            print(f"FAIL -- self-mutation 2 target line not found verbatim: {target2!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated2 = box_c.replace(target2, "else if (st == 0) {", 1)
        v2, _ = run_all(app_h, main_c, mutated2, gen12_c)
        if not any("pickup_ps < 0" in x for x in v2):
            print("FAIL -- self-mutation 2: dropping the pickup_ps < 0 guard did NOT "
                  "turn this test red (vacuous check)")
            return 1
        print("self-mutation 2: dropping the pickup_ps < 0 guard -- correctly caught:")
        for x in v2:
            if "pickup_ps < 0" in x:
                print(f"  (mutated-copy) FAIL: {x}")

        # --- self-mutation 3: move the clear call INSIDE pdna_gen12_mount() -------
        target3 = "  m->tgt.met_game = met_game;\n"
        if target3 not in gen12_c:
            print(f"FAIL -- self-mutation 3 target line not found verbatim: {target3!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated3 = gen12_c.replace(target3, target3 + "  app_box_resume_clear();\n", 1)
        v3 = check_clear_sites(main_c, mutated3)
        if not any("host-compiled mount function" in x for x in v3):
            print("FAIL -- self-mutation 3: calling app_box_resume_clear() from inside "
                  "pdna_gen12_mount() did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation 3: calling app_box_resume_clear() from inside the "
              "host-pure pdna_gen12_mount() -- correctly caught:")
        for x in v3:
            if "host-compiled mount function" in x:
                print(f"  (mutated-copy) FAIL: {x}")

        # --- self-mutation 4: drop the box-mismatch guard in app_box_resume_take ---
        target4 = "  if (g_resume_box != box) return -1;\n"
        if target4 not in main_c:
            print(f"FAIL -- self-mutation 4 target line not found verbatim: {target4!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated4 = main_c.replace(target4, "", 1)
        v4 = check_take_body_guards(mutated4)
        if not any("box mismatch" in x for x in v4):
            print("FAIL -- self-mutation 4: dropping the box-mismatch guard did NOT "
                  "turn this test red (vacuous check)")
            return 1
        print("self-mutation 4: dropping app_box_resume_take's box-mismatch guard -- "
              "correctly caught:")
        for x in v4:
            print("  (mutated-copy) FAIL:", x)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nhost_box_resume_test: ok (shipped source clean, all three mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
