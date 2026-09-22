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

    self_test_mutation_detection(lines)

    print(f"host_bank_meta_backup_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
