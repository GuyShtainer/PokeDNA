#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_xfer_view_readonly_test.py -- structural guard for BACKLOG #150 S150-15:
the GB ORIGINAL row's whole read path must never write anything. Same posture as
tests/host_gb_write_gate_test.py (:8-11) -- grep the shipped source, don't re-type
a copy of it.

Three checks:

  (i) source/xfer_view.c (comments stripped) contains NONE of the write-primitive
      names: sf_write_verified(, sf_save_rolling(, f_write(, f_unlink(, f_rename(,
      f_mkdir(, gbsc_add(, gbsc_remove(, gbsc_set_claimed(, bc_pack(,
      xr_migrate_once(, pdna_bank_next_serial(.

  (ii) gb_original_summary_open()'s own body in source/pdna_gen12.c calls
       native_summary_run( with a literal `false` as its second argument and
       `NULL`/`0` as its third, and contains no bc_pack(.

  (iii) app_view_original()'s own body in source/pdna_main.c contains none of the
        (i) names and no commit(. Skipped (not failed) until BACKLOG #150 S150-15
        step 3 adds the function -- this test file is created in step 2, before
        step 3's app_view_original exists.

Run directly:

    python3 tests/host_xfer_view_readonly_test.py

Registered in tests/run_host_tests.py's PY_TESTS list.
"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
XFER_VIEW_C = ROOT / "source" / "xfer_view.c"
PDNA_GEN12_C = ROOT / "source" / "pdna_gen12.c"
PDNA_MAIN_C = ROOT / "source" / "pdna_main.c"

WRITE_NAMES = [
    "sf_write_verified(", "sf_save_rolling(", "f_write(", "f_unlink(", "f_rename(",
    "f_mkdir(", "gbsc_add(", "gbsc_remove(", "gbsc_set_claimed(", "bc_pack(",
    "xr_migrate_once(", "pdna_bank_next_serial(",
]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    """Same brace-depth walk as host_gb_write_gate_test.py's own helper."""
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


def check_xfer_view_no_writes(text: str) -> list[str]:
    body = strip_comments(text)
    return [f"source/xfer_view.c: found write primitive {name!r}"
            for name in WRITE_NAMES if name in body]


def check_gb_original_summary_open(text: str) -> list[str]:
    body = strip_comments(extract_function_body(text, "gb_original_summary_open"))
    if not body:
        return ["gb_original_summary_open(): function body not found"]
    out = []
    if not re.search(r"native_summary_run\s*\([^,]*,\s*/\*[^*]*\*/\s*false", body) and \
       not re.search(r"native_summary_run\s*\([^,]*,\s*false", body):
        out.append("gb_original_summary_open(): does not call native_summary_run( with "
                    "a literal `false` as its second argument")
    if not (re.search(r"native_summary_run\s*\([^,]*,[^,]*,\s*/\*[^*]*\*/\s*NULL", body) or
            re.search(r"native_summary_run\s*\([^,]*,[^,]*,\s*NULL", body) or
            re.search(r"native_summary_run\s*\([^,]*,[^,]*,\s*0\b", body)):
        out.append("gb_original_summary_open(): does not call native_summary_run( with "
                    "NULL/0 as its third argument")
    if "bc_pack(" in body:
        out.append("gb_original_summary_open(): body contains bc_pack( -- a write path "
                    "leaked into the read-only entry point")
    return out


def check_app_view_original(text: str) -> list[str]:
    body = strip_comments(extract_function_body(text, "app_view_original"))
    if not body:
        return None  # not created yet -- step 3, SKIP not FAIL
    out = [f"app_view_original(): found write primitive {name!r}" for name in WRITE_NAMES if name in body]
    if re.search(r"\bcommit\s*\(", body):
        out.append("app_view_original(): body calls commit( -- nothing should be written here")
    return out


def run_all() -> tuple[list[str], bool]:
    violations = []
    violations += check_xfer_view_no_writes(XFER_VIEW_C.read_text())
    violations += check_gb_original_summary_open(PDNA_GEN12_C.read_text())
    step3_result = check_app_view_original(PDNA_MAIN_C.read_text())
    step3_present = step3_result is not None
    if step3_present:
        violations += step3_result
    return violations, step3_present


def main() -> int:
    if not XFER_VIEW_C.exists():
        print("SKIP (source/xfer_view.c not found)")
        return 0

    violations, step3_present = run_all()
    print(f"xfer_view.c write-primitive scan: {'ok' if not violations else 'see violations below'}")
    print(f"app_view_original() check: {'checked' if step3_present else 'SKIP (not created yet -- S150-15 step 3)'}")
    if violations:
        print("FAIL -- the GB ORIGINAL read path is not read-only:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: xfer_view.c calls no write primitive, gb_original_summary_open is "
          "structurally read-only" + (", app_view_original is clean" if step3_present else ""))

    # --- self-mutation 1: insert a write call into a scratch xfer_view.c -------
    tmpdir = Path(tempfile.mkdtemp(prefix="xferviewgate_"))
    try:
        scratch = tmpdir / "xfer_view.c"
        shutil.copyfile(XFER_VIEW_C, scratch)
        original = scratch.read_text()
        marker = "bool xv_has_original(const uint8_t rec80[80]) {"
        if marker not in original:
            print(f"FAIL -- self-mutation 1 target not found verbatim: {marker!r}")
            return 1
        mutated = original.replace(
            marker, marker + "\n  sf_write_verified(\"/tmp/x\", rec80, 0);", 1)
        scratch.write_text(mutated)
        v1 = check_xfer_view_no_writes(scratch.read_text())
        if not v1:
            print("FAIL -- self-mutation 1 (insert sf_write_verified( into xfer_view.c) "
                  "was NOT caught")
            return 1
        print("self-mutation 1: inserting sf_write_verified( into xfer_view.c -- correctly caught:")
        for v in v1:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- self-mutation 2: flip false -> true in gb_original_summary_open ----
        scratch2 = tmpdir / "pdna_gen12.c"
        shutil.copyfile(PDNA_GEN12_C, scratch2)
        orig2 = scratch2.read_text()
        target2 = "return native_summary_run(original80, /*can_edit*/false, /*out80*/NULL, note);"
        if target2 not in orig2:
            print(f"FAIL -- self-mutation 2 target not found verbatim: {target2!r} "
                  f"(source drifted -- update this test's target string)")
            return 1
        mutated2 = orig2.replace(
            target2,
            "return native_summary_run(original80, /*can_edit*/true, /*out80*/NULL, note);", 1)
        scratch2.write_text(mutated2)
        v2 = check_gb_original_summary_open(scratch2.read_text())
        if not v2:
            print("FAIL -- self-mutation 2 (flip false -> true) was NOT caught")
            return 1
        print("self-mutation 2: flipping can_edit false -> true in gb_original_summary_open "
              "-- correctly caught:")
        for v in v2:
            print(f"  (mutated-copy) FAIL: {v}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nxfer_view_readonly: ok (shipped source clean, both self-mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
