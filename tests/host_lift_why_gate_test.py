#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_lift_why_gate_test.py -- structural guard for BACKLOG #166 (b166): a
Gen-1/2 grid cell whose lift would be refused must not offer MOVE TO BOX at
all, and gb_can_lift_hook_impl (BoxSource.can_lift's real body) must not
re-derive its own copy of the refusal rules now that gb_lift_why_hook/
gb_lift_why_bs (source/pdna_gen12.c) already compute them.

Pure-text checks, no mgba, no build -- the same "grep the shipped source,
don't re-type your own copy of it" posture tests/host_gb_write_gate_test.py
already uses for a write-safety gate, applied here to a menu-row gate instead.

Two checks:

  (a) app_mon_menu_readonly (source/pdna_main.c): the block that builds the
      RO_MOVE row (anchored on `act[n++] = RO_MOVE`) must consult
      `g_src_ops->lift_why` before adding the row -- a version that adds
      RO_MOVE unconditionally off `g_src_ops->move` alone (the pre-#166 shape)
      must be caught.

  (b) gb_can_lift_hook_impl (source/pdna_gen12.c): its own body must call
      `gb_lift_why_bs(` -- delegating to the shared rule table, not
      re-implementing `gbs_box_writable`/`gbs_can_delete` a second time (the
      pre-#166 shape), which could silently diverge from the reason text
      AppSrcOps.lift_why shows.

Self-mutation: each check is re-run against a mutated copy reverted to the
pre-#166 shape, and must FAIL -- proving the check is not vacuously true.

Run directly:

    python3 tests/host_lift_why_gate_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (b166).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDNA_MAIN = ROOT / "source" / "pdna_main.c"
PDNA_GEN12 = ROOT / "source" / "pdna_gen12.c"


def strip_comments(text: str) -> str:
    """Blank out /* ... */ and // comments but keep every newline (line numbers
    in any future error message still point at real source). Tokens must be
    matched in CODE only -- a comment that happens to mention the right name
    must not satisfy a check the code itself no longer does."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    """Return the full body (signature through matching close-brace) of the
    top-level function `func_name` defined in `text`, or "" if not found.
    Brace-depth walk, not a fixed-line-count slice -- robust to the function
    growing or shrinking as other lanes land."""
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


def check_ro_move_gate(text: str) -> list[str]:
    """RO_MOVE (app_mon_menu_readonly, source/pdna_main.c) must be gated on
    lift_why, not offered off `g_src_ops->move` alone."""
    body = extract_function_body(text, "app_mon_menu_readonly")
    if not body:
        return ["app_mon_menu_readonly() not found in source/pdna_main.c"]
    body = strip_comments(body)
    if "act[n++] = RO_MOVE" not in body:
        return ["app_mon_menu_readonly() no longer builds an RO_MOVE row at all"]
    # The row-add line must sit textually AFTER a `lift_why` reference, and that
    # reference must be reachable on the same conditional path (both live inside
    # the same `if (g_src_ops && g_src_ops->move) { ... }` block in the shipped
    # source) -- checked here as "lift_why appears at all, before the row add",
    # which a version that offers the row unconditionally off `move` cannot pass.
    move_idx = body.index("act[n++] = RO_MOVE")
    lift_idx = body.find("lift_why")
    if lift_idx < 0:
        return ["app_mon_menu_readonly()'s RO_MOVE row never consults lift_why"]
    if lift_idx > move_idx:
        return ["app_mon_menu_readonly() consults lift_why AFTER already adding "
                "the RO_MOVE row -- too late to hide it"]
    # The row-add must be CONDITIONED on move_why, not merely preceded by the
    # call (re-verify 2026-09-21: `(void)move_why; { ... RO_MOVE }` passed).
    guard = re.search(r"if\s*\(\s*!\s*move_why\s*\)\s*\{[^}]*act\[n\+\+\]\s*=\s*RO_MOVE", body)
    if not guard:
        return ["app_mon_menu_readonly()'s RO_MOVE row is not actually gated on "
                "move_why -- lift_why is consulted but its result is ignored"]
    return []


def check_can_lift_delegates(text: str) -> list[str]:
    """gb_can_lift_hook_impl (source/pdna_gen12.c) must delegate to
    gb_lift_why_bs, not re-run gbs_box_writable/gbs_can_delete itself."""
    body = extract_function_body(text, "gb_can_lift_hook_impl")
    if not body:
        return ["gb_can_lift_hook_impl() not found in source/pdna_gen12.c"]
    body = strip_comments(body)
    if "gb_lift_why_bs(" not in body:
        return ["gb_can_lift_hook_impl() does not delegate to gb_lift_why_bs() -- "
                "it may be re-deriving its own copy of the refusal rules"]
    return []


def revert_ro_move_gate(text: str) -> str:
    """Mutated copy: the pre-#166 unconditional RO_MOVE row (offered off
    `g_src_ops->move` alone, no lift_why gate)."""
    old = (
        "    if (g_src_ops && g_src_ops->move) {\n"
        "      move_why = g_src_ops->lift_why ? g_src_ops->lift_why(rec) : 0;\n"
        "      if (!move_why) { lab[n] = PDNA_LBL_MOVE_TO_BOX; act[n++] = RO_MOVE; }\n"
        "    }\n"
    )
    new = "    if (g_src_ops && g_src_ops->move) { lab[n] = PDNA_LBL_MOVE_TO_BOX; act[n++] = RO_MOVE; }\n"
    assert text.count(old) == 1, "app_mon_menu_readonly's RO_MOVE block not found verbatim -- update the mutation"
    return text.replace(old, new)


def revert_can_lift_delegation(text: str) -> str:
    """Mutated copy: the pre-#166 gb_can_lift_hook_impl body, re-deriving the
    refusal directly instead of delegating to gb_lift_why_bs()."""
    old = (
        "static bool gb_can_lift_hook_impl(int box, int slot) {\n"
        "  return gb_lift_why_bs(box, slot) == NULL;\n"
        "}\n"
    )
    new = (
        "static bool gb_can_lift_hook_impl(int box, int slot) {\n"
        "  if (!g_ed) return false;\n"
        "  if (!(app_can_edit() && gbs_box_writable(&g_ed->s, box) == GBS_OK)) return false;\n"
        "  return gbs_can_delete(&g_ed->s, box, slot, g_ed->list, 0) == GBS_OK;\n"
        "}\n"
    )
    assert text.count(old) == 1, "gb_can_lift_hook_impl's delegating body not found verbatim -- update the mutation"
    return text.replace(old, new)


def main() -> int:
    main_text = PDNA_MAIN.read_text()
    gen12_text = PDNA_GEN12.read_text()

    failures: list[str] = []
    failures += check_ro_move_gate(main_text)
    failures += check_can_lift_delegates(gen12_text)
    for f in failures:
        print(f"  FAIL: {f}")
    if failures:
        print(f"host_lift_why_gate_test: {len(failures)} failure(s) on shipped source")
        return 1
    print("shipped source clean: RO_MOVE consults lift_why before adding its row, "
          "gb_can_lift_hook_impl delegates to gb_lift_why_bs")

    # Self-mutation: each check must FAIL against a reverted copy.
    mutated_main = revert_ro_move_gate(main_text)
    mut_failures = check_ro_move_gate(mutated_main)
    if not mut_failures:
        print("host_lift_why_gate_test: SELF-MUTATION FAILED -- reverting RO_MOVE's "
              "lift_why gate was not caught")
        return 1
    print(f"self-mutation check 1: reverting RO_MOVE's lift_why gate -- correctly caught:\n"
          f"  (mutated-copy) FAIL: {mut_failures[0]}")

    mutated_gen12 = revert_can_lift_delegation(gen12_text)
    mut_failures2 = check_can_lift_delegates(mutated_gen12)
    if not mut_failures2:
        print("host_lift_why_gate_test: SELF-MUTATION FAILED -- reverting "
              "gb_can_lift_hook_impl's delegation was not caught")
        return 1
    print(f"self-mutation check 2: reverting gb_can_lift_hook_impl's delegation -- correctly caught:\n"
          f"  (mutated-copy) FAIL: {mut_failures2[0]}")

    print("host_lift_why_gate_test: ok (shipped source clean, both mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
