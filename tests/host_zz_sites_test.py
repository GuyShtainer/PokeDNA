#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_zz_sites_test.py -- structural pins for lane zz (BACKLOG #383 #384 #385 #377 #382).
Pure text checks against the shipped source (no build). Every fact function is shared by the real
check and by in-memory MUTATIONS that must turn it red (self-test in main()).

  #384  app_xfer_pid_rekey rewrites the copy's header key (gbsc_set_file_key, checked against gbsc_file_key) BEFORE the
        verified write of the new name, and unlinks the old name only after that write -- never zero copies.
  #385  xfer_reconcile_bank_phase2 counts identity-only Bank hits (xrc_bank_match(..., true, ..)) where a box has no first-8
        hit, xfer_reconcile_classify_all hands them to xrc_classify, and the detail line is wired.
  #383  app_xfer_save_now promotes ONLY when a transfer is pending ("nothing pending" == ok), a REAL failed
        promotion (pending, rewrite failed) still gives ok = false (#175 D1).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip_comments(text: str) -> str:
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i:i + 2] == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text[i:i + 2] == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def function_body(text: str, name: str) -> str:
    text = strip_comments(text)
    m = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.MULTILINE)
    if not m:
        return ""
    depth, j = 1, m.end()
    while j < len(text) and depth > 0:
        depth += (text[j] == "{") - (text[j] == "}")
        j += 1
    return text[m.start():j]


def mutate(body: str, old: str, new: str) -> str:
    assert old in body, f"mutation anchor not found: {old!r}"
    return body.replace(old, new, 1)


# ---------------------------------------------------------------------------------------------- #383
def f383(body: str) -> tuple[bool, str]:
    if not re.search(r"bool\s+had_pending\s*=\s*app_xfer_pending\(\)\s*;", body):
        return False, "app_xfer_save_now: had_pending = app_xfer_pending() missing (#383)"
    if not re.search(r"bool\s+promoted\s*=\s*!had_pending\s*\|\|\s*app_xfer_promote\(\)\s*;", body):
        return False, "app_xfer_save_now: promoted must be `!had_pending || app_xfer_promote()` (#383)"
    if not re.search(r"ok\s*=\s*promoted\s*;", body):
        return False, "`ok = promoted;` missing (#175 D1)"
    return True, "ok"


# ---------------------------------------------------------------------------------------------- #384
def f384(body: str) -> tuple[bool, str]:
    i_rd = body.find("sf_read_full(plan->old_path")
    i_key = body.find("gbsc_set_file_key(buf, len, new_key)")
    i_chk = body.find("gbsc_file_key(buf, len) == new_key")
    i_wr = body.find("sf_write_verified(plan->new_path, buf, len)")
    i_ul = body.find("f_unlink(plan->old_path)")
    if min(i_rd, i_key, i_chk, i_wr, i_ul) < 0:
        return False, "app_xfer_pid_rekey: read / set_file_key / file_key check / verified write / unlink not all present (#384)"
    if not (i_rd < i_key < i_wr < i_ul and i_key < i_chk < i_wr):
        return False, "app_xfer_pid_rekey: order must be read, header key rewrite (+check), verified write of the new name, unlink old (#384)"
    if "gbsc_key_from_path(plan->new_path" not in body:
        return False, "app_xfer_pid_rekey: the key must come from the new path (#384)"
    return True, "ok"


# ---------------------------------------------------------------------------------------------- #385
def f385(phase2: str, classify: str, detail: str) -> tuple[bool, str]:
    if not re.search(r"if\s*\(\s*!by_identity\s*&&\s*m\s*==\s*0\s*&&\s*h->direction\s*==\s*XR_DIR_ABROAD_G3\s*&&\s*h->bank_ident_matches\s*<\s*2\s*\)", phase2):
        return False, "phase2: the identity-only scan guard (!by_identity && m == 0 && ABROAD_G3 && < 2) is missing (#385)"
    if not re.search(r"xrc_bank_match\(recs,\s*&e2,\s*true,\s*&islot\)", phase2):
        return False, "phase2: the identity scan must call xrc_bank_match(recs, &e2, true, ..) (#385)"
    if not re.search(r"in\.bank_ident_matches\s*=\s*h->bank_ident_matches\s*;", classify):
        return False, "classify_all: bank_ident_matches not passed to xrc_classify (#385)"
    if "case XRC_IN_BANK" not in detail or "PDNA_XRC_D_IN_BANK" not in detail:
        return False, "xrc_detail_line: XRC_IN_BANK has no detail line (#385)"
    return True, "ok"


def run() -> None:
    main_t = (SRC / "pdna_main.c").read_text()
    p2 = function_body(main_t, "xfer_reconcile_bank_phase2")
    ca = function_body(main_t, "xfer_reconcile_classify_all")
    dl = function_body(main_t, "xrc_detail_line")
    ok, d = f385(p2, ca, dl)
    check(ok, d)
    for label, which, old, new in (
        ("385: identity scan guard dropped", "p2", "!by_identity && m == 0 &&", "false &&"),
        ("385: identity scan uses the first-8 match", "p2", "xrc_bank_match(recs, &e2, true, &islot)", "xrc_bank_match(recs, &e2, false, &islot)"),
        ("385: classify_all forgets the count", "ca", "in.bank_ident_matches = h->bank_ident_matches;", ""),
        ("385: detail line unwired", "dl", "case XRC_IN_BANK", "case XRC_G3HOME_DUP"),
    ):
        parts = {"p2": p2, "ca": ca, "dl": dl}
        if old not in parts[which]:
            check(False, f"mutation anchor missing ({label})")
            continue
        parts[which] = mutate(parts[which], old, new)
        ok, _ = f385(parts["p2"], parts["ca"], parts["dl"])
        check(not ok, f"MUT {label} was NOT caught")
    rk = function_body(main_t, "app_xfer_pid_rekey")
    check(bool(rk), "app_xfer_pid_rekey body not found")
    ok, d = f384(rk)
    check(ok, d)
    for label, old, new in (
        ("384: header rewrite dropped", "gbsc_set_file_key(buf, len, new_key) == 0", "true"),
        ("384: unlink the old name before the verified write", "sf_write_verified(plan->new_path, buf, len) == SF_OK;", "f_unlink(plan->old_path) == FR_OK;"),
    ):
        if old not in rk:
            check(False, f"mutation anchor missing ({label})")
            continue
        ok, _ = f384(mutate(rk, old, new))
        check(not ok, f"MUT {label} was NOT caught")
    b = function_body(main_t, "app_xfer_save_now")
    check(bool(b), "app_xfer_save_now body not found")
    ok, d = f383(b)
    check(ok, d)
    for label, old, new in (
        ("383: unconditional promote again", "bool promoted = !had_pending || app_xfer_promote();", "bool promoted = app_xfer_promote();"),
        ("383: nothing pending treated as a failure", "!had_pending || app_xfer_promote()", "had_pending && app_xfer_promote()"),
    ):
        if old not in b:
            check(False, f"mutation anchor missing ({label})")
            continue
        ok, _ = f383(mutate(b, old, new))
        check(not ok, f"MUT {label} was NOT caught")


def main() -> int:
    run()
    print(f"host_zz_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
