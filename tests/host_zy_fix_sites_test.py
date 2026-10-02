#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_zy_fix_sites_test.py -- lane zy fix-pass structural pins (D1, D2, D3 call site, D4, D5, D7).

pdna_bank.c / pdna_box.c do not compile on the host, so these are text-level pins on the REAL source
(the style of host_bank_meta_backup_sites_test.py). Each pin has a MUTANT: the same text with the fix
reverted must make the pin fail, or the pin is decoration.

  D1  box_load records `g_box_unread = bsrc == BML_BOX_READ_ERROR;` before `g_loaded = box;`;
      box_save refuses on g_box_unread (before it can reach sf_save_rolling_ok);
      migrate_flat_pk3 clears it; pdna_bank_flush_deletions keeps the deletion queued.
  D2  clear_origin only memsets the origin slot when it still holds the carried mon (memcmp s_held, 8).
  D3  meta_load heals a bank.meta.tmp recovery through bml_meta_heal_tmp (rename), never leaving the
      truncating meta_save() path armed for it (g_meta_from_bak cleared only on a successful heal).
  D4  banksrc_commit shows PDNA_BANK_META_* when meta_save() fails after a good box save.
  D5  bank_backup_v1 calls box_load(b) on an absent primary BEFORE the absent-skip f_stat.
  D7  every bml_box_heal / bml_meta_heal_tmp call is bracketed by rmbl_pause()/rmbl_resume().
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BANK = (ROOT / "source" / "pdna_bank.c").read_text()
BOX = (ROOT / "source" / "pdna_box.c").read_text()

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return "\n".join(ln.split("//")[0] for ln in text.split("\n"))


def func(text: str, sig_re: str) -> str:
    lines = strip(text).split("\n")
    start = next((i for i, ln in enumerate(lines) if re.search(sig_re, ln)), None)
    if start is None:
        return ""
    depth, seen = 0, False
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        seen = seen or "{" in lines[i]
        if seen and depth == 0:
            return "\n".join(lines[start:i + 1])
    return ""


def p_d1(bank: str, box: str) -> str | None:
    bl = func(bank, r"^static bool box_load\(int box\) \{")
    if not re.search(r"g_box_unread\s*=\s*bsrc\s*==\s*BML_BOX_READ_ERROR\s*;\s*\n\s*g_loaded\s*=\s*box\s*;", bl):
        return "box_load: no `g_box_unread = bsrc == BML_BOX_READ_ERROR;` right before `g_loaded = box;`"
    bs = func(bank, r"^static bool box_save\(void\) \{")
    m = re.search(r"if\s*\(\s*g_box_unread\s*\)\s*\{[^}]*return false;\s*\}", bs)
    if not m or bs.find("sf_save_rolling_ok") < m.start():
        return "box_save: no `if (g_box_unread) { ... return false; }` ahead of the write"
    mg = func(bank, r"^static void migrate_flat_pk3\(void\)|^.*migrate_flat_pk3\(void\) \{")
    if not re.search(r"g_box_unread\s*=\s*false\s*;\s*\n\s*g_loaded\s*=\s*0\s*;", mg):
        return "migrate_flat_pk3: g_box_unread not cleared beside g_loaded = 0"
    fd = func(bank, r"^int pdna_bank_flush_deletions\(void\) \{")
    if not re.search(r"if\s*\(\s*g_box_unread\s*\)\s*\{[^}]*kept\+\+;\s*continue;", fd, flags=re.S):
        return "pdna_bank_flush_deletions: an unread box does not keep the deletion queued"
    return None


def p_d2(bank: str, box: str) -> str | None:
    co = func(box, r"^static uint8_t\* clear_origin\(BoxSource\* src, int box\) \{")
    if not re.search(r"if\s*\(\s*memcmp\(\s*o \+ \(uint32_t\)s_orig_slot \* 80,\s*s_held,\s*8\s*\)\s*==\s*0\s*\)\s*memset\(\s*o \+ \(uint32_t\)s_orig_slot \* 80,\s*0,\s*80\s*\)", co):
        return "clear_origin: the origin slot is cleared without checking it still holds the carried mon"
    return None


def p_d3(bank: str, box: str) -> str | None:
    ml = func(bank, r"^static bool __attribute__\(\(noinline\)\) meta_load\(void\) \{")
    if "bml_meta_heal_tmp(" not in ml:
        return "meta_load: no bml_meta_heal_tmp call (a bank.meta.tmp recovery would be healed by a truncating write)"
    if not re.search(r"g_meta_from_bak\s*=\s*from_bak\s*;", ml) or not re.search(r"if\s*\(\s*healed\s*\)\s*\{\s*g_meta_from_bak\s*=\s*false", ml):
        return "meta_load: g_meta_from_bak must stay set until the rename heal succeeded"
    if "g_meta_state = 2" not in ml.split("bml_meta_heal_tmp")[1]:
        return "meta_load: a failed .tmp heal must make meta_save refuse (state 2)"
    return None


def p_d4(bank: str, box: str) -> str | None:
    bc = func(bank, r"^static bool banksrc_commit\(void\) \{")
    if not re.search(r"if\s*\(\s*!meta_save\(\)\s*&&\s*ok\s*\)\s*\n?\s*msg_wait\(\s*PDNA_BANK_META_TITLE,\s*UI_WARN,\s*PDNA_BANK_META_L1,\s*PDNA_BANK_META_L2\s*\)", bc):
        return "banksrc_commit: a failed meta_save is silent"
    return None


def p_d5(bank: str, box: str) -> str | None:
    bb = func(bank, r"^static bool __attribute__\(\(noinline\)\) bank_backup_v1\(void\) \{")
    m = re.search(r"if\s*\(\s*f_stat\(\s*src,\s*0\s*\)\s*==\s*FR_NO_FILE\s*\)\s*\(void\)box_load\(b\);", bb)
    skip = bb.find("f_stat(src, &sfno)")
    if not m or skip < 0 or m.start() > skip:
        return "bank_backup_v1: no heal-load of an absent primary before the absent-skip"
    return None


def p_d7(bank: str, box: str) -> str | None:
    s = strip(bank)
    for call in ("bml_box_heal(", "bml_meta_heal_tmp("):
        i = s.find(call)
        if i < 0:
            return f"{call} call not found"
        pre, post = s[max(0, i - 120):i], s[i:i + 160]
        if "rmbl_pause();" not in pre or "rmbl_resume();" not in post:
            return f"{call} is not bracketed by rmbl_pause()/rmbl_resume()"
    return None


PINS = [
    ("D1", p_d1, [
        (BANK, "  g_box_unread = bsrc == BML_BOX_READ_ERROR;\n", ""),
        (BANK, "  if (g_box_unread) {\n    log_line(\"bank: box %02d was not read", "  if (0) {\n    log_line(\"bank: box %02d was not read"),
        (BANK, "  g_box_unread = false;\n  g_loaded = 0;", "  g_loaded = 0;"),
        (BANK, "    if (g_box_unread) {                              /* Zy D1", "    if (0) {                              /* Zy D1"),
    ]),
    ("D2", p_d2, [(BOX, "if (memcmp(o + (uint32_t)s_orig_slot * 80, s_held, 8) == 0) memset(", "memset(")]),
    ("D3", p_d3, [
        (BANK, "bml_meta_heal_tmp(PDNA_BANK_DIR, META_BYTES)", "false"),
        (BANK, "      g_meta_state = 2;\n      log_line(\"bank: meta heal from .tmp failed", "      log_line(\"bank: meta heal from .tmp failed"),
    ]),
    ("D4", p_d4, [(BANK, "if (!meta_save() && ok)", "meta_save();\n  if (0)")]),
    ("D5", p_d5, [(BANK, "if (f_stat(src, 0) == FR_NO_FILE) (void)box_load(b);", "")]),
    ("D7", p_d7, [
        (BANK, "      rmbl_pause();\n      healed = bml_box_heal(", "      healed = bml_box_heal("),
        (BANK, "    rmbl_pause();\n    bool healed = bml_meta_heal_tmp(", "    bool healed = bml_meta_heal_tmp("),
    ]),
]


def main() -> int:
    for name, fn, muts in PINS:
        err = fn(BANK, BOX)
        check(err is None, f"{name}: {err}")
        for k, (src, old, new) in enumerate(muts):
            if old not in src:
                check(False, f"{name} mutant {k}: anchor text not found (pin and source drifted)")
                continue
            if src is BANK:
                err = fn(src.replace(old, new, 1), BOX)
            else:
                err = fn(BANK, src.replace(old, new, 1))
            check(err is not None, f"{name} mutant {k} NOT caught")
    print(f"host_zy_fix_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
