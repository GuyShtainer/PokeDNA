#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_item_pick_test.py -- structural guard for BACKLOG #195 (Gen-1/2 bag: real
item names + a per-pocket filter + a real picker instead of a raw numeric ID prompt).

source/pdna_pick.c does not compile on the host (tonc/libgba UI drawing calls pull in
headers the pure-C host tests deliberately avoid), same reason tests/host_gb_move_pick_test.py
(the brief's own template for this file) is a text-level check instead of a compiled
unit test. Three things must all hold:

  (a) Both GB ADD ITEM sites (source/pdna_gbbag.c's Gen-1 screen, source/pdna_gbpack.c's
      Gen-2 screen) call pick_item( -- NOT num_entry_opt("ITEM ID" -- and that call is
      bracketed by pick_item_set_gen1_2(GBIN_GEN1/GBIN_GEN2, ...) immediately before it
      and pick_item_set_gen1_2(0, GBF_G_RED, 0) immediately after (review D2: `game` is
      now a real, threaded GbGame, not merely the generation -- gbb_pocket_of() answers
      differently for Gold/Silver vs Crystal at four ids).
  (b) The Gen-3 bag's own ADD ITEM site (source/pdna_bag.c, the brief's own "parity
      reference") keeps ceiling 0 -- its pick_item( call is NOT bracketed by any
      pick_item_set_gen1_2*( call, so its behaviour with ceiling 0 stays exactly what it
      was before this lane touched pdna_pick.c at all.
  (c) The restricted picker's row-label helper (item_label_for in pdna_pick.c) labels
      through gb_item_label() when a generation is set, not merely a raw "#n" (the OLD
      unconditional behaviour this lane replaced).

Run directly:

    python3 tests/host_gb_item_pick_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #195).
"""
import re
import sys
import tempfile
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PICK_C = ROOT / "source" / "pdna_pick.c"
GBBAG_C = ROOT / "source" / "pdna_gbbag.c"
GBPACK_C = ROOT / "source" / "pdna_gbpack.c"
BAG_C = ROOT / "source" / "pdna_bag.c"

CALL_RE = re.compile(r"\bpick_item\s*\(")
DEF_RE = re.compile(r"\buint16_t\s+pick_item\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def find_call_lines(text: str) -> list[int]:
    code = strip_comments(text)
    lines = code.splitlines()
    out = []
    for i, line in enumerate(lines, 1):
        if DEF_RE.search(line):
            continue
        if CALL_RE.search(line):
            out.append(i)
    return out


def check_no_raw_id_prompt(name: str, text: str) -> list[str]:
    if '"ITEM ID"' in text:
        return [f"{name}: still has a raw num_entry_opt(\"ITEM ID\" prompt -- "
                f"BACKLOG #195 F2 replaced it with the picker"]
    return []


def check_bracketed_gen(name: str, text: str, gen_token: str) -> list[str]:
    """Every pick_item( call site is bracketed by pick_item_set_gen1_2(<gen_token>, ...)
    within the 3 lines immediately before it (the ADD ITEM sites also call
    pick_item_set_gen1_2_cat( in between, BACKLOG #195 F2's own pre-filter) and
    pick_item_set_gen1_2(0, GBF_G_RED, 0) immediately after (review D2's own 3-arg
    signature -- `game` is a real, threaded GbGame now, not merely the generation)."""
    code = strip_comments(text)
    lines = code.splitlines()
    violations = []
    sites = find_call_lines(text)
    if not sites:
        return [f"{name}: no pick_item( call site found at all"]
    for lineno in sites:
        before_window = "\n".join(lines[max(0, lineno - 4):lineno - 1]).replace(" ", "")
        after = lines[lineno] if lineno < len(lines) else ""
        has_set = f"pick_item_set_gen1_2({gen_token}," in before_window
        has_clear = "pick_item_set_gen1_2(0,GBF_G_RED,0)" in after.replace(" ", "")
        if not (has_set and has_clear):
            violations.append(f"{name}:{lineno}: pick_item( call is not bracketed by "
                              f"pick_item_set_gen1_2({gen_token}, ...) (within the 3 lines "
                              f"before it) and pick_item_set_gen1_2(0, GBF_G_RED, 0) "
                              f"(immediately after)")
    return violations


def check_unbracketed(name: str, text: str) -> list[str]:
    """The Gen-3 (ceiling 0) site must NOT be bracketed by any pick_item_set_gen1_2*( call."""
    code = strip_comments(text)
    lines = code.splitlines()
    violations = []
    for lineno in find_call_lines(text):
        before = lines[lineno - 2] if lineno >= 2 else ""
        after = lines[lineno] if lineno < len(lines) else ""
        if "pick_item_set_gen1_2" in before or "pick_item_set_gen1_2" in after:
            violations.append(f"{name}:{lineno}: the Gen-3 bag's pick_item( call is "
                              f"unexpectedly bracketed -- ceiling 0 must stay every existing "
                              f"caller's default (constraint: byte-identical Gen-3 behaviour)")
    return violations


def check_label_uses_gb_item_label(pick_c: str) -> list[str]:
    m = re.search(r"static void item_label_for\([^)]*\)\s*\{", pick_c)
    if not m:
        return ["pdna_pick.c: item_label_for() not found"]
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
    if "gb_item_label(" not in body:
        return ["pdna_pick.c: item_label_for() never calls gb_item_label() -- BACKLOG #195 F1's "
                "real-name upgrade has no effect (still \"#n\"-only)"]
    if "g_item_gen" not in body:
        return ["pdna_pick.c: item_label_for() calls gb_item_label() unconditionally -- the "
                "g_item_gen == 0 raw \"#n\" mode (held-item fields) would silently change"]
    return []


def run_all() -> list[str]:
    pick_c = PICK_C.read_text()
    gbbag_c = GBBAG_C.read_text()
    gbpack_c = GBPACK_C.read_text()
    bag_c = BAG_C.read_text()

    v = []
    v += check_no_raw_id_prompt("pdna_gbbag.c", gbbag_c)
    v += check_no_raw_id_prompt("pdna_gbpack.c", gbpack_c)
    v += check_bracketed_gen("pdna_gbbag.c", gbbag_c, "GBIN_GEN1")
    v += check_bracketed_gen("pdna_gbpack.c", gbpack_c, "GBIN_GEN2")
    v += check_unbracketed("pdna_bag.c", bag_c)
    v += check_label_uses_gb_item_label(pick_c)
    return v


def main() -> int:
    if not (PICK_C.exists() and GBBAG_C.exists() and GBPACK_C.exists() and BAG_C.exists()):
        print("SKIP (source files not found)")
        return 0

    violations = run_all()
    if violations:
        print("FAIL -- BACKLOG #195 item-picker gap:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: both GB ADD ITEM sites call the real picker (no raw ID prompt), bracketed by "
          "the right generation; the Gen-3 bag's own site stays unbracketed (ceiling 0); "
          "item_label_for() labels through gb_item_label() when a generation is set")

    tmpdir = Path(tempfile.mkdtemp(prefix="gbitempick_"))
    try:
        # --- self-mutation 1: re-introduce the raw "ITEM ID" prompt in pdna_gbpack.c ---
        gbpack_c = GBPACK_C.read_text()
        target1 = 'uint16_t id16 = pick_item(1);'
        if target1 not in gbpack_c:
            print(f"FAIL -- self-mutation 1 target line not found verbatim: {target1!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated1 = gbpack_c.replace(
            target1,
            'uint16_t id16; num_entry_opt("ITEM ID", 1, 999, (uint32_t*)&id16);', 1)
        m1 = check_no_raw_id_prompt("pdna_gbpack.c", mutated1)
        if not m1:
            print("FAIL -- self-mutation 1: reintroducing the raw ITEM ID prompt did NOT "
                  "turn this test red (vacuous check)")
            return 1
        print("self-mutation 1: raw ITEM ID prompt back in pdna_gbpack.c -- correctly caught:")
        for mv in m1:
            print(f"  (mutated-copy) FAIL: {mv}")

        # --- self-mutation 2: drop the bracket around pdna_gbbag.c's pick_item( call ------
        gbbag_c = GBBAG_C.read_text()
        target2 = "      pick_item_set_gen1_2(GBIN_GEN1, GBF_G_RED, gbb_max_item_id(GBF_G_RED));\n"
        if target2 not in gbbag_c:
            print(f"FAIL -- self-mutation 2 target line not found verbatim: {target2!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated2 = gbbag_c.replace(target2, "", 1)
        m2 = check_bracketed_gen("pdna_gbbag.c", mutated2, "GBIN_GEN1")
        if not m2:
            print("FAIL -- self-mutation 2: dropping the pick_item_set_gen1_2( bracket did NOT "
                  "turn this test red (vacuous check)")
            return 1
        print("self-mutation 2: dropping pdna_gbbag.c's pick_item_set_gen1_2 bracket -- "
              "correctly caught:")
        for mv in m2:
            print(f"  (mutated-copy) FAIL: {mv}")

        # --- self-mutation 3: item_label_for() stops reading g_item_gen (always "#n") ------
        pick_c = PICK_C.read_text()
        target3 = "    if (g_item_gen && gb_item_label(g_item_gen, (uint8_t)id, out, cap)) return;\n"
        if target3 not in pick_c:
            print(f"FAIL -- self-mutation 3 target line not found verbatim: {target3!r} "
                  f"(source drifted -- update this test)")
            return 1
        mutated3 = pick_c.replace(target3, "", 1)
        m3 = check_label_uses_gb_item_label(mutated3)
        if not m3:
            print("FAIL -- self-mutation 3: removing the gb_item_label() call did NOT turn "
                  "this test red (vacuous check)")
            return 1
        print("self-mutation 3: removing item_label_for()'s gb_item_label() call -- "
              "correctly caught:")
        for mv in m3:
            print(f"  (mutated-copy) FAIL: {mv}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nhost_gb_item_pick_test: ok (shipped source clean, all three mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
