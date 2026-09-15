#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_itemmap_gen_test.py -- structural guard that the checked-in
source/item_map_g2g3.{c,h} equal a FRESH run of tools/gen_item_map.py
(BACKLOG #150 S150-8-CORE, decision 10 / D-Q1).

Pure Python (no compile), tests/host_xfer_rekey_order_test.py-style: same
FAILURES / check() shape, stdlib only, run directly with `python3`. Wired into
tests/run_host_tests.py's PY_TESTS list (decision 9) -- appended at the END,
never replacing another lane's entry.

Four things, all load-bearing:

  1. `python3 tools/gen_item_map.py --check` exits 0.
  2. `--emit-stdout c` / `--emit-stdout h` byte-match the checked-in files
     (the same comparison --check makes internally, re-proven here from a
     subprocess so a bug in --check's own exit-code plumbing cannot hide a
     real drift).
  3. Mutation proof (a guard that cannot fail is not a guard): change one
     table entry in the emitted `.c` text in memory and assert the comparison
     in (2) would now FAIL on it.
  4. PYTHONHASHSEED independence: the emitted `.c` text is byte-identical
     across PYTHONHASHSEED=0, PYTHONHASHSEED=1 and the parent's own
     (unset/random) environment -- any difference means the drift guard is a
     flake waiting to be "fixed" by deletion.

Run directly:

    python3 tests/host_itemmap_gen_test.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "tools" / "gen_item_map.py"
OUT_C = ROOT / "source" / "item_map_g2g3.c"
OUT_H = ROOT / "source" / "item_map_g2g3.h"

FAILURES: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        FAILURES.append(msg)
        print(f"  !! FAIL: {msg}")


def run(*args: str, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(GEN), *args],
        cwd=ROOT, capture_output=True, text=True, env=env,
    )


def test_check_exits_zero() -> None:
    print("== (1) `--check` exits 0 on the checked-in files ==")
    r = run("--check")
    check(r.returncode == 0, f"gen_item_map.py --check exited {r.returncode}, stdout:\n{r.stdout}\nstderr:\n{r.stderr}")
    check("item map: OK (107 pairs)" in r.stdout, f"--check did not print the expected OK line; stdout was:\n{r.stdout}")


def emitted(mode: str) -> str:
    r = run("--emit-stdout", mode)
    if r.returncode != 0:
        raise AssertionError(f"--emit-stdout {mode} exited {r.returncode}: {r.stderr}")
    return r.stdout


def test_emit_matches_checked_in() -> tuple[str, str]:
    print("== (2) `--emit-stdout c`/`h` byte-match the checked-in files ==")
    c_text = emitted("c")
    h_text = emitted("h")
    on_disk_c = OUT_C.read_text(encoding="utf-8")
    on_disk_h = OUT_H.read_text(encoding="utf-8")
    check(c_text == on_disk_c, "emitted source/item_map_g2g3.c differs from the checked-in file")
    check(h_text == on_disk_h, "emitted source/item_map_g2g3.h differs from the checked-in file")
    return c_text, h_text


def test_mutation_proof(c_text: str) -> None:
    print("== (3) mutation proof: a hand-edited entry must FAIL the byte-compare ==")
    # Locate the kG2ToG3[] array body (between its declaration line and the
    # first bare "};") and flip the first nonzero table entry's value --
    # e.g. the mutation the brief itself demonstrates by hand: LIGHT BALL's
    # 202 -> 200. Any single-digit change here must make the byte-compare fail.
    decl = "static const uint16_t kG2ToG3[ITEM_MAP_G2_MAX + 1] = {\n"
    check(c_text.count(decl) == 1, f"expected exactly one {decl!r} in the emitted .c text")
    start = c_text.index(decl) + len(decl)
    end = c_text.index("\n};\n", start)
    body = c_text[start:end]

    tokens = body.split(",")
    mutated_one = False
    for i, tok in enumerate(tokens):
        stripped = tok.strip()
        if stripped.isdigit() and int(stripped) != 0:
            new_val = int(stripped) + 1
            tokens[i] = tok.replace(stripped, str(new_val), 1)
            mutated_one = True
            break
    check(mutated_one, "mutation harness did not find a nonzero table entry to mutate -- test itself is broken")
    mutated_body = ",".join(tokens)
    mutated_text = c_text[:start] + mutated_body + c_text[end:]
    check(mutated_text != c_text, "mutation produced identical text -- test itself is broken")

    on_disk_c = OUT_C.read_text(encoding="utf-8")
    check(mutated_text != on_disk_c,
          "mutated text still equals the checked-in file -- the mutation is vacuous, this guard could never fail")


def test_hashseed_independence() -> None:
    print("== (4) PYTHONHASHSEED independence ==")
    import os
    base_out = emitted("c")
    envs = []
    for seed in ("0", "1"):
        env = dict(os.environ)
        env["PYTHONHASHSEED"] = seed
        r = run("--emit-stdout", "c", env=env)
        check(r.returncode == 0, f"PYTHONHASHSEED={seed}: exited {r.returncode}: {r.stderr}")
        envs.append((seed, r.stdout))
    for seed, out in envs:
        check(out == base_out, f"PYTHONHASHSEED={seed} output differs from the default-env output")
    # also cross-compare the two seeded runs directly
    if len(envs) == 2:
        check(envs[0][1] == envs[1][1], "PYTHONHASHSEED=0 and PYTHONHASHSEED=1 outputs differ from each other")


def main() -> int:
    test_check_exits_zero()
    c_text, _h_text = test_emit_matches_checked_in()
    test_mutation_proof(c_text)
    test_hashseed_independence()

    print()
    if FAILURES:
        print(f"host_itemmap_gen_test: {len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("host_itemmap_gen_test: all checks passed")
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
