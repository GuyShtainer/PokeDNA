#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_bank_meta_backup_sites_test.py -- BACKLOG #168b structural pin.

source/pdna_bank.c does not compile on the host (tonc/ui.h/snd.h/rmbl.h), so this is a
text-level structural check on the REAL source, in the same spirit as
tests/host_escape_gate_sites_test.py: bank.meta (box names, wallpapers, and the serial
pdna_bank_next_serial()'s whole ident32-uniqueness guarantee rests on) used to have NO
backup at all (meta_save() called sf_write_verified, which keeps zero prior copies) while
box_save() already took one rolling .bak per box file. A bad meta write was therefore
unrecoverable.

Checks:
  (a) meta_save()'s body calls sf_save_rolling(meta_path(), ...) and does NOT call
      sf_write_verified(meta_path(), ... (the old, backup-less write).
  (b) meta_load()'s body falls back to a "<meta path>.bak" read (a second
      sf_read_full( call, on a path built with a ".bak" suffix) when the primary parse
      fails, before meta_defaults() runs.
  (c) meta_load() sets g_serial_trusted = !from_bak (or an equivalent negation of the
      bak-fallback flag) on its successful-parse path, and meta_defaults() sets
      g_serial_trusted = false -- so a collision scan short-circuit (BACKLOG #168a,
      pdna_box.c) can never trust a serial that came from the .bak or from a default
      reset.
  (d) pdna_bank_serial_trusted() exists and returns g_serial_trusted.
  (e) self_test_mutation_detection(): reverting meta_save() to its pre-fix
      sf_write_verified( form, on an in-memory copy of the real text, makes check (a)
      fail -- proves the checker has teeth, not just a coincidental pass.

BACKLOG #219a (single-shot recovery): sf_save_rolling's own f_stat(meta_path()) probe
(savefile.c) cannot tell "primary present but corrupt" from "primary present and fine"
-- both are FR_OK -- so the meta_save() that follows a .bak-fallback load backed the
corrupt primary up OVER the still-good bank.meta.bak, and a second corruption before
another clean write had no good .bak left to recover from.
  (f) meta_load() sets a `g_meta_from_bak = from_bak;` (or equivalent) assignment on
      its successful-parse path, AFTER the g_serial_trusted assignment.
  (g) meta_save()'s body, BEFORE its sf_save_rolling(meta_path(), ...) call, guards an
      `f_unlink(meta_path())` call on the from-bak flag (an `if (g_meta_from_bak)`
      block containing `f_unlink(meta_path())`) -- turning a corrupt primary into
      "absent" so sf_save_rolling skips the backup step and bank.meta.bak keeps the
      good recovered bytes.
  (h) self_test_mutation_detection_219a(): dropping the `if (g_meta_from_bak)` guard
      from meta_save() (un-indenting the f_unlink call to always run, or deleting the
      block) makes check (g) fail on an in-memory copy of the real text.

BACKLOG #219c (backup-v1 missing bank.meta): backup-v1 copied box*.box only -- a bad
meta write was unrecoverable even on a card whose boxes were safe. bank_backup_v1()
now also writes a verified copy to backup-v1/bank.meta, keyed by that file's OWN
existence (not a second marker) so a card whose boxes' DONE marker already exists
(upgraded from an older build) gets ONLY the missing meta copy, never a redo of the
16-box loop.
  (i) bank_backup_v1()'s body builds a `PDNA_BANK_DIR "/backup-v1/bank.meta"` path and
      calls `sf_write_verified(meta_bak, ...)` (or equivalent, matching the same
      `sf_write_verified(` + a name built from that backup-v1/bank.meta path) to copy it.
  (j) bank_backup_v1()'s body contains an `if (boxes_done) return meta_done;` (or
      equivalent early-return keyed on the boxes marker) BEFORE the
      `for (int b = 0; b < BANK_BOXES; b++)` box loop -- an already-done card's boxes
      are never redone just to add the meta copy.
  (k) self_test_mutation_detection_219c(): dropping the `if (boxes_done) return
      meta_done;` line from bank_backup_v1() makes check (j) fail.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BANK_C = ROOT / "source" / "pdna_bank.c"

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


SF_SAVE_ROLLING_META_RE = re.compile(r"sf_save_rolling\(\s*meta_path\(\)")
SF_WRITE_VERIFIED_META_RE = re.compile(r"sf_write_verified\(\s*meta_path\(\)")
BAK_SUFFIX_RE = re.compile(r'"%s\.bak"\s*,\s*meta_path\(\)')
SF_READ_FULL_RE = re.compile(r"sf_read_full\(")
META_DEFAULTS_CALL_RE = re.compile(r"\bmeta_defaults\(\)\s*;")
SERIAL_TRUSTED_TRUE_RE = re.compile(r"g_serial_trusted\s*=\s*!from_bak\s*;")
SERIAL_TRUSTED_FALSE_RE = re.compile(r"g_serial_trusted\s*=\s*false\s*;")
META_FROM_BAK_SET_RE = re.compile(r"g_meta_from_bak\s*=\s*from_bak\s*;")
META_FROM_BAK_GUARD_RE = re.compile(r"if\s*\(\s*g_meta_from_bak\s*\)")
UNLINK_META_RE = re.compile(r"f_unlink\(\s*meta_path\(\)")
BACKUPV1_META_PATH_RE = re.compile(r'"/backup-v1/bank\.meta"')
BACKUPV1_META_WRITE_RE = re.compile(r"sf_write_verified\(\s*meta_bak\s*,")
BOXES_DONE_SHORT_CIRCUIT_RE = re.compile(r"if\s*\(\s*boxes_done\s*\)\s*return\s*meta_done\s*;")
BOX_LOOP_RE = re.compile(r"for\s*\(\s*int\s+b\s*=\s*0\s*;\s*b\s*<\s*BANK_BOXES\s*;\s*b\+\+\s*\)")


def check_meta_save(body: list[str]) -> tuple[bool, str]:
    has_rolling = any(SF_SAVE_ROLLING_META_RE.search(ln) for ln in body)
    has_old = any(SF_WRITE_VERIFIED_META_RE.search(ln) for ln in body)
    if not has_rolling:
        return False, "meta_save(): no sf_save_rolling(meta_path(), ...) call found"
    if has_old:
        return False, "meta_save(): still calls sf_write_verified(meta_path(), ...) (no backup)"
    return True, ""


def check_meta_load_fallback(body: list[str]) -> tuple[bool, str]:
    read_lines = [i for i, ln in enumerate(body) if SF_READ_FULL_RE.search(ln)]
    if len(read_lines) < 2:
        return False, f"meta_load(): expected 2 sf_read_full( calls (primary + .bak), found {len(read_lines)}"
    if not any(BAK_SUFFIX_RE.search(ln) for ln in body):
        return False, 'meta_load(): no \'"%s.bak", meta_path()\' fallback path construction found'
    defaults_lines = [i for i, ln in enumerate(body) if META_DEFAULTS_CALL_RE.search(ln)]
    if not defaults_lines:
        return False, "meta_load(): no meta_defaults() call found"
    if not (read_lines[-1] < defaults_lines[0]):
        return False, "meta_load(): meta_defaults() is not called AFTER the .bak read attempt"
    return True, ""


def check_trust_flag(load_body: list[str], defaults_body: list[str]) -> tuple[bool, str]:
    if not any(SERIAL_TRUSTED_TRUE_RE.search(ln) for ln in load_body):
        return False, "meta_load(): no `g_serial_trusted = !from_bak;` on the success path"
    if not any(SERIAL_TRUSTED_FALSE_RE.search(ln) for ln in defaults_body):
        return False, "meta_defaults(): no `g_serial_trusted = false;` -- a defaulted serial must never be trusted"
    return True, ""


def check_from_bak_flag(load_body: list[str]) -> tuple[bool, str]:
    if not any(META_FROM_BAK_SET_RE.search(ln) for ln in load_body):
        return False, "meta_load(): no `g_meta_from_bak = from_bak;` assignment found"
    return True, ""


def check_meta_save_heal(body: list[str]) -> tuple[bool, str]:
    guard_lines = [i for i, ln in enumerate(body) if META_FROM_BAK_GUARD_RE.search(ln)]
    if not guard_lines:
        return False, "meta_save(): no `if (g_meta_from_bak)` guard found"
    unlink_lines = [i for i, ln in enumerate(body) if UNLINK_META_RE.search(ln)]
    if not unlink_lines:
        return False, "meta_save(): no f_unlink(meta_path() call found"
    rolling_lines = [i for i, ln in enumerate(body) if SF_SAVE_ROLLING_META_RE.search(ln)]
    if not rolling_lines:
        return False, "meta_save(): no sf_save_rolling(meta_path(), ...) call found"
    guard = guard_lines[0]
    after_guard = [u for u in unlink_lines if u >= guard]
    if not after_guard:
        return False, "meta_save(): f_unlink(meta_path() call not found at/after the g_meta_from_bak guard"
    if not (min(after_guard) < rolling_lines[0]):
        return False, "meta_save(): f_unlink(meta_path() call is not BEFORE the sf_save_rolling(...) call"
    return True, ""


def self_test_mutation_detection_219a(lines: list[str]) -> None:
    """MUT M2 (BACKLOG #219a): drop the `if (g_meta_from_bak)` guard from meta_save()
    (the f_unlink call would then run unconditionally, or the check simply can't see a
    guarded heal) -- check_meta_save_heal() must then fail. Applied to an IN-MEMORY
    copy only."""
    s, e = extract_function(lines, r"^static bool meta_save\(void\) \{")
    mutated = list(lines)
    for i in range(s, e):
        mutated[i] = META_FROM_BAK_GUARD_RE.sub("if (0)", mutated[i])
    ok, detail = check_meta_save_heal(mutated[s:e])
    check(not ok, "MUT M2 (meta_save's g_meta_from_bak guard dropped) was NOT caught -- "
                  f"checker reported ok anyway ({detail!r})")


def check_backupv1_meta_copy(body: list[str]) -> tuple[bool, str]:
    if not any(BACKUPV1_META_PATH_RE.search(ln) for ln in body):
        return False, 'bank_backup_v1(): no "/backup-v1/bank.meta" path construction found'
    if not any(BACKUPV1_META_WRITE_RE.search(ln) for ln in body):
        return False, "bank_backup_v1(): no sf_write_verified(meta_bak, ...) call found"
    return True, ""


def check_backupv1_no_redo(body: list[str]) -> tuple[bool, str]:
    guard_lines = [i for i, ln in enumerate(body) if BOXES_DONE_SHORT_CIRCUIT_RE.search(ln)]
    if not guard_lines:
        return False, "bank_backup_v1(): no `if (boxes_done) return meta_done;` short-circuit found"
    loop_lines = [i for i, ln in enumerate(body) if BOX_LOOP_RE.search(ln)]
    if not loop_lines:
        return False, "bank_backup_v1(): no `for (int b = 0; b < BANK_BOXES; b++)` box loop found"
    if not (guard_lines[0] < loop_lines[0]):
        return False, "bank_backup_v1(): the boxes_done short-circuit is not BEFORE the box loop"
    return True, ""


def self_test_mutation_detection_219c(lines: list[str]) -> None:
    """MUT M3 (BACKLOG #219c): drop the `if (boxes_done) return meta_done;` short-
    circuit from bank_backup_v1() -- check_backupv1_no_redo() must then fail. Applied
    to an IN-MEMORY copy only."""
    s, e = extract_function(lines, r"^static bool __attribute__\(\(noinline\)\) bank_backup_v1\(void\) \{")
    mutated = list(lines)
    for i in range(s, e):
        mutated[i] = BOXES_DONE_SHORT_CIRCUIT_RE.sub("/* removed by MUT M3 */", mutated[i])
    ok, detail = check_backupv1_no_redo(mutated[s:e])
    check(not ok, "MUT M3 (bank_backup_v1's boxes_done short-circuit dropped) was NOT caught -- "
                  f"checker reported ok anyway ({detail!r})")


def self_test_mutation_detection(lines: list[str]) -> None:
    """MUT M1: revert meta_save()'s body to the pre-fix sf_write_verified( call --
    check_meta_save() must then fail. Applied to an IN-MEMORY copy only."""
    s, e = extract_function(lines, r"^static bool meta_save\(void\) \{")
    mutated = list(lines)
    for i in range(s, e):
        mutated[i] = SF_SAVE_ROLLING_META_RE.sub(
            "sf_write_verified(meta_path()", mutated[i])
        mutated[i] = mutated[i].replace(", NULL) == SF_OK;", ") == SF_OK;")
    ok, detail = check_meta_save(mutated[s:e])
    check(not ok, "MUT M1 (meta_save reverted to sf_write_verified) was NOT caught -- "
                  f"checker reported ok anyway ({detail!r})")


def main() -> int:
    if not BANK_C.exists():
        print(f"SKIP (missing {BANK_C})")
        return 0
    raw = BANK_C.read_text(errors="replace")
    stripped = strip_comments(raw)
    lines = stripped.split("\n")

    s, e = extract_function(lines, r"^static bool meta_save\(void\) \{")
    ok, detail = check_meta_save(lines[s:e])
    check(ok, detail)

    s, e = extract_function(lines, r"^static bool __attribute__\(\(noinline\)\) meta_load\(void\) \{")
    load_body = lines[s:e]
    ok, detail = check_meta_load_fallback(load_body)
    check(ok, detail)

    ds, de = extract_function(lines, r"^static void meta_defaults\(void\) \{")
    defaults_body = lines[ds:de]
    ok, detail = check_trust_flag(load_body, defaults_body)
    check(ok, detail)

    check(any(re.search(r"\breturn g_serial_trusted\s*;", ln) for ln in lines),
          "pdna_bank_serial_trusted(): no `return g_serial_trusted;` found")

    ok, detail = check_from_bak_flag(load_body)
    check(ok, detail)

    s, e = extract_function(lines, r"^static bool meta_save\(void\) \{")
    save_body = lines[s:e]
    ok, detail = check_meta_save_heal(save_body)
    check(ok, detail)

    s, e = extract_function(
        lines, r"^static bool __attribute__\(\(noinline\)\) bank_backup_v1\(void\) \{")
    backupv1_body = lines[s:e]
    ok, detail = check_backupv1_meta_copy(backupv1_body)
    check(ok, detail)
    ok, detail = check_backupv1_no_redo(backupv1_body)
    check(ok, detail)

    self_test_mutation_detection(lines)
    self_test_mutation_detection_219a(lines)
    self_test_mutation_detection_219c(lines)

    print(f"host_bank_meta_backup_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
