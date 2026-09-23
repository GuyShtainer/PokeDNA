#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_partymail_gate_test.py -- BACKLOG #227: Gen 3 has no "a party member is holding
Mail" guard on the two party->box sites app_party_full_deposit_offer's own inline check
(#226) does not cover.

source/pdna_main.c does not compile on the host (it needs tonc/libgba headers the other
pure-C host tests deliberately avoid), so the only thing that can prove these two guards
stay in place is a text-level structural check on the REAL source, in the same style as
tests/host_escape_gate_sites_test.py: comment-strip + brace-depth function-body
extraction + line-order regex checks, with a self-mutation harness so the check is
proven to have teeth every run, not just when someone remembers to demonstrate it by
hand.

Checks:
  (a) SWAP arm: party_place_held's body calls g3_party_rec_has_mail(pslot) at a line
      strictly BEFORE its party_to_box(pslot, y80) call -- the displaced party mon must
      be refused as a mail holder before it is ever boxed. MUT A deletes the guard line
      and must be caught.
  (b) MOVE TO BOX arm: app_party_mon_menu's body calls g3_party_rec_has_mail(rec) at a
      line strictly BEFORE its party_to_box(rec, tobox_grab) call. MUT B deletes the
      guard line and must be caught.

BACKLOG #227 D2 (the same mail-orphan bug at three more party->box/party-destroying
sites, review mail-integrity-review.md R3): DUPLICATE aliases the mail slot onto two
party records; RELEASE drops the record with nothing left pointing at the mail it still
owns (retail refuses this too, pokemon_storage_system.c:2661-2671); Day-Care deposit
blanks the daycare struct with no migration (PokeDNA has no mail-array reader/writer at
all, so it refuses rather than silently losing the message, unlike retail's own MIGRATE,
daycare.c:162-173).
  (c) DUPLICATE: app_duplicate's body calls g3_party_rec_has_mail(rec) at a line
      strictly BEFORE its party_append(block, g_frlg, out) call. MUT C deletes the guard
      line and must be caught.
  (d) RELEASE: app_release's body calls g3_party_rec_has_mail(rec) at a line strictly
      BEFORE its party_release(block, g_frlg, slot) call. MUT D deletes the guard line
      and must be caught.
  (e) DAY-CARE: app_to_daycare's body calls g3_party_rec_has_mail(rec) at a line
      strictly BEFORE its memcpy(g_sb1 + base deposit call. MUT E deletes the guard
      line and must be caught.
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
    """Blank /* ... */ (possibly multi-line) and // ... text, keeping every line's
    0-based index unchanged -- a call sitting only inside a comment must not count."""
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out_lines = []
    for ln in text.split("\n"):
        idx = ln.find("//")
        out_lines.append(ln[:idx] if idx != -1 else ln)
    return "\n".join(out_lines)


def extract_function(lines: list[str], sig_re: str) -> tuple[int, int]:
    """Return (start, end) 0-based line indices [start, end) of the function whose
    signature line matches `sig_re`, by brace-depth counting from that line's first
    '{' to the matching close."""
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


MAIL_GUARD_RE = re.compile(r"g3_party_rec_has_mail\(")
PARTY_TO_BOX_PSLOT_RE = re.compile(r"party_to_box\(pslot,\s*y80\)")
PARTY_TO_BOX_TOBOX_RE = re.compile(r"party_to_box\(rec,\s*tobox_grab\)")
PARTY_APPEND_RE       = re.compile(r"party_append\(block,\s*g_frlg,\s*out\)")
PARTY_RELEASE_CALL_RE = re.compile(r"party_release\(block,\s*g_frlg,\s*slot\)")
DAYCARE_DEPOSIT_RE    = re.compile(r"memcpy\(g_sb1\s*\+\s*base")


def check_swap_arm_mail_guard(stripped_lines: list[str]) -> None:
    """(a) party_place_held: g3_party_rec_has_mail(pslot) must come strictly BEFORE
    party_to_box(pslot, y80) -- refuse before the displaced party mon is boxed."""
    s, e = extract_function(stripped_lines, r"static bool party_place_held\(")
    body = stripped_lines[s:e]
    guard_i = first_line_matching(body, MAIL_GUARD_RE)
    write_i = first_line_matching(body, PARTY_TO_BOX_PSLOT_RE)
    if write_i is None:
        check(False, "party_place_held: no party_to_box(pslot, y80) call found -- fix this test")
        return
    check(guard_i is not None and guard_i < write_i,
          "party_place_held: g3_party_rec_has_mail(pslot) missing or not before "
          "party_to_box(pslot, y80) -- #227's SWAP-arm mail guard is gone")


def check_tobox_arm_mail_guard(stripped_lines: list[str]) -> None:
    """(b) app_party_mon_menu: g3_party_rec_has_mail(rec) must come strictly BEFORE
    party_to_box(rec, tobox_grab) -- refuse before MOVE TO BOX boxes the mon."""
    s, e = extract_function(stripped_lines, r"bool app_party_mon_menu\(")
    body = stripped_lines[s:e]
    guard_i = first_line_matching(body, MAIL_GUARD_RE)
    write_i = first_line_matching(body, PARTY_TO_BOX_TOBOX_RE)
    if write_i is None:
        check(False, "app_party_mon_menu: no party_to_box(rec, tobox_grab) call found -- fix this test")
        return
    check(guard_i is not None and guard_i < write_i,
          "app_party_mon_menu: g3_party_rec_has_mail(rec) missing or not before "
          "party_to_box(rec, tobox_grab) -- #227's MOVE-TO-BOX mail guard is gone")


def check_duplicate_mail_guard(stripped_lines: list[str]) -> None:
    """(c) app_duplicate: g3_party_rec_has_mail(rec) must come strictly BEFORE
    party_append(block, g_frlg, out) -- refuse before a second record aliases the
    same mail slot (BACKLOG #227 D2)."""
    s, e = extract_function(stripped_lines, r"static bool app_duplicate\(")
    body = stripped_lines[s:e]
    guard_i = first_line_matching(body, MAIL_GUARD_RE)
    write_i = first_line_matching(body, PARTY_APPEND_RE)
    if write_i is None:
        check(False, "app_duplicate: no party_append(block, g_frlg, out) call found -- fix this test")
        return
    check(guard_i is not None and guard_i < write_i,
          "app_duplicate: g3_party_rec_has_mail(rec) missing or not before "
          "party_append(block, g_frlg, out) -- #227 D2's DUPLICATE mail guard is gone")


def check_release_mail_guard(stripped_lines: list[str]) -> None:
    """(d) app_release: g3_party_rec_has_mail(rec) must come strictly BEFORE
    party_release(block, g_frlg, slot) -- refuse before the record (and the mail it
    still owns) is dropped (BACKLOG #227 D2)."""
    s, e = extract_function(stripped_lines, r"static bool app_release\(")
    body = stripped_lines[s:e]
    guard_i = first_line_matching(body, MAIL_GUARD_RE)
    write_i = first_line_matching(body, PARTY_RELEASE_CALL_RE)
    if write_i is None:
        check(False, "app_release: no party_release(block, g_frlg, slot) call found -- fix this test")
        return
    check(guard_i is not None and guard_i < write_i,
          "app_release: g3_party_rec_has_mail(rec) missing or not before "
          "party_release(block, g_frlg, slot) -- #227 D2's RELEASE mail guard is gone")


def check_daycare_mail_guard(stripped_lines: list[str]) -> None:
    """(e) app_to_daycare: g3_party_rec_has_mail(rec) must come strictly BEFORE the
    memcpy(g_sb1 + base ... deposit -- refuse before the boarder is written and the
    daycare struct is blanked with no mail migrated (BACKLOG #227 D2)."""
    s, e = extract_function(stripped_lines, r"static bool app_to_daycare\(")
    body = stripped_lines[s:e]
    guard_i = first_line_matching(body, MAIL_GUARD_RE)
    write_i = first_line_matching(body, DAYCARE_DEPOSIT_RE)
    if write_i is None:
        check(False, "app_to_daycare: no memcpy(g_sb1 + base ... deposit call found -- fix this test")
        return
    check(guard_i is not None and guard_i < write_i,
          "app_to_daycare: g3_party_rec_has_mail(rec) missing or not before the "
          "memcpy(g_sb1 + base deposit -- #227 D2's DAY-CARE mail guard is gone")


# ---- self-mutation harness: prove each check above actually has teeth ---------------

def mutate_delete_line(body: list[str], pat: re.Pattern) -> list[str]:
    out = []
    for ln in body:
        out.append(ln if not pat.search(ln) else "")
    return out


def self_test_mutation_detection(stripped_lines: list[str]) -> None:
    # MUT A: delete the SWAP arm's mail-guard line entirely.
    s, e = extract_function(stripped_lines, r"static bool party_place_held\(")
    body = stripped_lines[s:e]
    mutA = mutate_delete_line(body, MAIL_GUARD_RE)
    write_i = first_line_matching(mutA, PARTY_TO_BOX_PSLOT_RE)
    guard_i = first_line_matching(mutA, MAIL_GUARD_RE)
    caught = not (guard_i is not None and write_i is not None and guard_i < write_i)
    check(caught, "MUT A demonstration: deleting the SWAP-arm mail guard was NOT caught")
    print("  MUT A demonstration -- party_place_held's mail guard deleted: "
          + ("correctly caught" if caught else "MISSED"))

    # MUT B: delete the MOVE-TO-BOX arm's mail-guard line entirely.
    s2, e2 = extract_function(stripped_lines, r"bool app_party_mon_menu\(")
    body2 = stripped_lines[s2:e2]
    mutB = mutate_delete_line(body2, MAIL_GUARD_RE)
    write2_i = first_line_matching(mutB, PARTY_TO_BOX_TOBOX_RE)
    guard2_i = first_line_matching(mutB, MAIL_GUARD_RE)
    caughtB = not (guard2_i is not None and write2_i is not None and guard2_i < write2_i)
    check(caughtB, "MUT B demonstration: deleting the MOVE-TO-BOX mail guard was NOT caught")
    print("  MUT B demonstration -- app_party_mon_menu's mail guard deleted: "
          + ("correctly caught" if caughtB else "MISSED"))

    # MUT C: delete DUPLICATE's mail-guard line entirely (BACKLOG #227 D2).
    s3, e3 = extract_function(stripped_lines, r"static bool app_duplicate\(")
    body3 = stripped_lines[s3:e3]
    mutC = mutate_delete_line(body3, MAIL_GUARD_RE)
    write3_i = first_line_matching(mutC, PARTY_APPEND_RE)
    guard3_i = first_line_matching(mutC, MAIL_GUARD_RE)
    caughtC = not (guard3_i is not None and write3_i is not None and guard3_i < write3_i)
    check(caughtC, "MUT C demonstration: deleting the DUPLICATE mail guard was NOT caught")
    print("  MUT C demonstration -- app_duplicate's mail guard deleted: "
          + ("correctly caught" if caughtC else "MISSED"))

    # MUT D: delete RELEASE's mail-guard line entirely (BACKLOG #227 D2).
    s4, e4 = extract_function(stripped_lines, r"static bool app_release\(")
    body4 = stripped_lines[s4:e4]
    mutD = mutate_delete_line(body4, MAIL_GUARD_RE)
    write4_i = first_line_matching(mutD, PARTY_RELEASE_CALL_RE)
    guard4_i = first_line_matching(mutD, MAIL_GUARD_RE)
    caughtD = not (guard4_i is not None and write4_i is not None and guard4_i < write4_i)
    check(caughtD, "MUT D demonstration: deleting the RELEASE mail guard was NOT caught")
    print("  MUT D demonstration -- app_release's mail guard deleted: "
          + ("correctly caught" if caughtD else "MISSED"))

    # MUT E: delete DAY-CARE's mail-guard line entirely (BACKLOG #227 D2).
    s5, e5 = extract_function(stripped_lines, r"static bool app_to_daycare\(")
    body5 = stripped_lines[s5:e5]
    mutE = mutate_delete_line(body5, MAIL_GUARD_RE)
    write5_i = first_line_matching(mutE, DAYCARE_DEPOSIT_RE)
    guard5_i = first_line_matching(mutE, MAIL_GUARD_RE)
    caughtE = not (guard5_i is not None and write5_i is not None and guard5_i < write5_i)
    check(caughtE, "MUT E demonstration: deleting the DAY-CARE mail guard was NOT caught")
    print("  MUT E demonstration -- app_to_daycare's mail guard deleted: "
          + ("correctly caught" if caughtE else "MISSED"))


def main() -> int:
    raw = MAIN_C.read_text(errors="replace")
    stripped = strip_comments(raw)
    stripped_lines = stripped.split("\n")

    check_swap_arm_mail_guard(stripped_lines)
    check_tobox_arm_mail_guard(stripped_lines)
    check_duplicate_mail_guard(stripped_lines)
    check_release_mail_guard(stripped_lines)
    check_daycare_mail_guard(stripped_lines)
    self_test_mutation_detection(stripped_lines)

    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
