#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_xfer_registered_rom_test.py -- BACKLOG #269: a Gen-3 -> Gen-1 transfer falsely
refused "Put yellow.gb here" although the ROM was registered (Settings > Game ROM).

Root cause: gb_gen1_locate_rom() (source/pdna_gen12.c, the base-stats ROM probe every
Gen-1 paste/bank-down runs) looked ONLY beside the .sav. The registered ROM
(app_gb_rom_path(GB_GEN1)) -- the one box-sprite extraction and CREATE already used --
was never consulted. pdna_gen12.c does not compile on the host, so this is a text-
structural check on the REAL source (same style as host_createparty_gate_test.py) with
a self-mutation proof.

  (c1) the SD branch of gb_gen1_locate_rom calls app_gb_rom_path(GB_GEN1);
  (c2) that call comes BEFORE gb_rom_base_path() (registered wins, beside-save falls back);
  (c3) the registered arm opens the ROM as GB_ROM_GEN1 and only then sets romgs_ready.
MUT: deleting the app_gb_rom_path line (the old body) must be caught by (c1).
"""
from __future__ import annotations
import re, sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "source" / "pdna_gen12.c"
fails: list[str] = []
checks = 0


def check(c: bool, m: str) -> None:
    global checks
    checks += 1
    if not c:
        fails.append(m)


def strip(t: str) -> str:
    t = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), t, flags=re.S)
    return "\n".join(l.split("//")[0] for l in t.split("\n"))


def func(text: str, sig: str) -> str:
    i = text.index(sig)
    j = text.index("{", i)
    d = 0
    for k in range(j, len(text)):
        d += (text[k] == "{") - (text[k] == "}")
        if d == 0:
            return text[i:k + 1]
    raise AssertionError("unbalanced")


def verdicts(body: str) -> tuple[bool, bool, bool]:
    else_i = body.index("#else")
    sd = body[else_i:]
    reg = sd.find("app_gb_rom_path(GB_GEN1)")
    base = sd.find("gb_rom_base_path()")
    open_ = sd.find("GB_ROM_GEN1", reg) if reg >= 0 else -1
    ready = sd.find("romgs_ready = true", reg) if reg >= 0 else -1
    return reg >= 0, (reg >= 0 and base > reg), (open_ >= 0 and ready > open_ and ready < base)


def main() -> int:
    body = func(strip(SRC.read_text(errors="replace")), "gb_gen1_locate_rom(void)")
    c1, c2, c3 = verdicts(body)
    check(c1, "gb_gen1_locate_rom SD branch never consults app_gb_rom_path(GB_GEN1)")
    check(c2, "registered ROM is not tried before the beside-the-save probe")
    check(c3, "registered arm does not open as GB_ROM_GEN1 before marking romgs_ready")
    mut = body.replace("app_gb_rom_path(GB_GEN1)", "0")
    m1, _, _ = verdicts(mut)
    check(not m1, "MUT (registered lookup removed) was NOT caught")
    print("  MUT registered-lookup removed:", "caught" if not m1 else "MISSED")
    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print("  !! FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
