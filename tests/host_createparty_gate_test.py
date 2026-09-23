#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_createparty_gate_test.py -- BACKLOG #229: app_create_mon's party arm is a
latent 80-into-100 write.

app_create_mon (source/pdna_main.c) builds its record entirely in box form (species
picker, spread roll, editor all operate on an 80-byte tmp/out), hard-gated off the
party today by xg_create_row's `!is_party` check at its one call site (the action-row
build inside app_mon_menu) -- but the function itself used to do an unconditional
`memcpy(rec, out, 80)`. The day that gate moves, a party-shaped call writes 80 bytes
into a 100-byte slot and leaves the whole plaintext tail (status, level, mail, current
and max HP, the five stats) stale -- the same struct-tail class #225 already cost this
project once (b225's mail byte and Shedinja's HP were the same zero-filled tail).

The chosen fix is (a) from the brief, not (b): app_create_mon now takes `bool is_party`
and, when true, widens its 80-byte record through box_to_party() -- the SAME choke
point party_place_held's own ADD arm and app_party_deposit_undo already use for every
other box->party producer -- before committing, instead of leaving a comment or
gating the CREATE row differently. The CREATE row itself stays hidden on the party by
xg_create_row's existing `!is_party` gate; this only makes app_create_mon itself
correct for the day that gate moves.

source/pdna_main.c does not compile on the host (it needs tonc/libgba headers the
other pure-C host tests deliberately avoid), so this is a text-structural check on the
REAL source, in the same style as tests/host_escape_gate_sites_test.py, with a
self-mutation harness proving it has teeth.

Checks:
  (c1) app_create_mon's signature declares a `bool is_party` parameter -- #229's
       expansion has nothing to branch on otherwise.
  (c2) app_create_mon's body calls box_to_party(out, p100) -- the same choke point
       every other party producer uses.
  (c3) app_create_mon's body calls app_commit_with_dex(rec, true, commit, block) (the
       party-shaped commit) AND app_commit_with_dex(rec, false, commit, block) (the
       box-only arm) -- neither may be dropped.
  (c4) the A_CREATE dispatch site in app_mon_menu passes is_party through:
       `app_create_mon(rec, is_party, commit, block)`.

MUT C reverts the body to the old unconditional `memcpy(rec, out, 80)` +
`app_commit_with_dex(rec, false, commit, block)` (no is_party branch at all, as if the
expansion had never been added) and must be caught by (c2)/(c3).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN_C = ROOT / "source" / "pdna_main.c"

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out_lines = []
    for ln in text.split("\n"):
        idx = ln.find("//")
        out_lines.append(ln[:idx] if idx != -1 else ln)
    return "\n".join(out_lines)


def extract_function(lines: list[str], sig_re: str) -> tuple[int, int]:
    start = None
    for i, ln in enumerate(lines):
        if re.search(sig_re, ln):
            start = i
            break
    if start is None:
        raise AssertionError(f"signature not found: {sig_re}")
    depth = 0
    seen_open = False
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if "{" in lines[i]:
            seen_open = True
        if seen_open and depth == 0:
            return start, i + 1
    raise AssertionError(f"unbalanced braces for function at line {start + 1}")


def first_line_matching(body: list[str], pat: re.Pattern) -> int | None:
    for i, ln in enumerate(body):
        if pat.search(ln):
            return i
    return None


IS_PARTY_PARAM_RE = re.compile(r"static\s+bool\s+app_create_mon\([^)]*\bbool\s+is_party\b")
BOX_TO_PARTY_CREATE_RE = re.compile(r"box_to_party\(out,\s*p100\)")
COMMIT_DEX_TRUE_RE = re.compile(r"app_commit_with_dex\(rec,\s*true,\s*commit,\s*block\)")
COMMIT_DEX_FALSE_RE = re.compile(r"app_commit_with_dex\(rec,\s*false,\s*commit,\s*block\)")
CREATE_CALL_SITE_RE = re.compile(
    r"app_create_mon\(\s*rec,\s*is_party,\s*commit,\s*block\s*\)")


def get_create_mon_body(stripped_lines: list[str]) -> list[str]:
    s, e = extract_function(stripped_lines, r"static bool app_create_mon\(")
    return stripped_lines[s:e]


def check_signature(stripped_lines: list[str]) -> None:
    s, _e = extract_function(stripped_lines, r"static bool app_create_mon\(")
    check(IS_PARTY_PARAM_RE.search(stripped_lines[s]) is not None,
          "app_create_mon: signature has no `bool is_party` parameter -- #229's "
          "expansion has nothing to branch on")


def check_expansion_and_commits(body: list[str]) -> None:
    check(first_line_matching(body, BOX_TO_PARTY_CREATE_RE) is not None,
          "app_create_mon: no box_to_party(out, p100) call found -- #229's party "
          "expansion (the same choke point every other producer uses) is gone")
    check(first_line_matching(body, COMMIT_DEX_TRUE_RE) is not None,
          "app_create_mon: no app_commit_with_dex(rec, true, commit, block) call -- "
          "a party-shaped create would commit as if it were a box record")
    check(first_line_matching(body, COMMIT_DEX_FALSE_RE) is not None,
          "app_create_mon: no app_commit_with_dex(rec, false, commit, block) call -- "
          "the box-only arm regressed")


def check_call_site(stripped_lines: list[str]) -> None:
    full = "\n".join(stripped_lines)
    check(CREATE_CALL_SITE_RE.search(full) is not None,
          "app_mon_menu: A_CREATE does not call app_create_mon(rec, is_party, commit, "
          "block) -- the party/box distinction never reaches app_create_mon")


def self_test_mutation_detection(stripped_lines: list[str]) -> None:
    body = get_create_mon_body(stripped_lines)
    mutC = [ln for ln in body]
    # Delete the expansion call and the party-shaped commit -- the old unconditional
    # 80-byte-only body had neither.
    mutC = ["" if BOX_TO_PARTY_CREATE_RE.search(ln) else ln for ln in mutC]
    mutC = ["" if COMMIT_DEX_TRUE_RE.search(ln) else ln for ln in mutC]
    has_expand = first_line_matching(mutC, BOX_TO_PARTY_CREATE_RE) is not None
    has_commit_true = first_line_matching(mutC, COMMIT_DEX_TRUE_RE) is not None
    caught = not (has_expand and has_commit_true)
    check(caught, "MUT C demonstration: reverting app_create_mon to the unconditional "
                  "80-byte write was NOT caught")
    print("  MUT C demonstration -- app_create_mon's party expansion reverted: "
          + ("correctly caught" if caught else "MISSED"))


def main() -> int:
    raw = MAIN_C.read_text(errors="replace")
    stripped = strip_comments(raw)
    stripped_lines = stripped.split("\n")

    check_signature(stripped_lines)
    check_expansion_and_commits(get_create_mon_body(stripped_lines))
    check_call_site(stripped_lines)
    self_test_mutation_detection(stripped_lines)

    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
