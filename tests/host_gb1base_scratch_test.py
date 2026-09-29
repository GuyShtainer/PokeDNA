#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_gb1base_scratch_test.py -- BACKLOG #276: CREATE > FROM SCRATCH on a Gen-1 save must
take its base row from the generated table BEFORE any card access.

gb_create_src_scratch (source/pdna_gen12.c) is static and GBA-only (FatFs), so it cannot
compile on the host: this is a text-structural check on the REAL source, in the style of
tests/host_createparty_gate_test.py, with a self-mutation harness proving it has teeth.
The pure half (the table's values, the accessor contract, "no I/O in the accessor" -- it
takes no reader) is tests/host_gb1base_test.c.

Checks, on the body of gb_create_src_scratch:
  (s1) the Gen-1 branch calls gb1_base_table_fill(dex, src) and returns true on success;
  (s2) that call comes BEFORE every card-touching call in the function (s_busy_reading,
       gb_create_locate_rom, gb_create_base1, f_open, f_read) -- so with the table linked
       the arm touches no file and needs no registered ROM;
  (s3) the ROM fallback is still there after it (gb_create_locate_rom + gb_create_base1),
       for a build without the generated table;
  (s4) gb_create_hook still only offers the LEGIT/SCRATCH prompt when a ROM is available
       (gb_create_rom_available gate), i.e. with no ROM the scratch arm runs silently.
MUT-A moves the table call after the ROM locate (s2 must fail); MUT-B drops it (s1 must
fail); MUT-C removes the gb_create_rom_available gate (s4 must fail).
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "source" / "pdna_gen12.c"
CARD = ("s_busy_reading", "gb_create_locate_rom", "gb_create_base1", "f_open", "f_read")


def body(text, name):
    m = re.search(r"\b" + name + r"\(uint16_t dex, GbNewMonSrc\* src\) \{", text)
    assert m, f"{name} not found"
    i, depth = m.end(), 1
    while depth:
        c = text[i]
        depth += (c == "{") - (c == "}")
        i += 1
    return text[m.end():i]


def check(text):
    errs = []
    b = body(text, "gb_create_src_scratch")
    code = re.sub(r"/\*.*?\*/", "", b, flags=re.S)
    call = code.find("gb1_base_table_fill(dex, src)")
    if call < 0 or not re.search(r"if \(gb1_base_table_fill\(dex, src\)\) return true;", code):
        errs.append("s1: no `if (gb1_base_table_fill(dex, src)) return true;` in the scratch arm")
    else:
        for c in CARD:
            k = code.find(c + "(")
            if k >= 0 and k < call:
                errs.append(f"s2: {c}() precedes the table lookup (card access before the table)")
    if "gb_create_locate_rom(GB_GEN1)" not in code or "gb_create_base1(dex, &sp)" not in code:
        errs.append("s3: the ROM fallback (locate + base1) is gone")
    hook = text[text.index("static bool gb_create_hook(void)"):]
    hook = hook[:hook.index("\n}\n")]
    if not re.search(r"if \(gb_create_rom_available\(\)\) \{\s*GbCreateMode mode = gb_create_origin_screen", hook):
        errs.append("s4: the LEGIT/SCRATCH prompt is no longer gated on gb_create_rom_available()")
    return errs


def main():
    text = SRC.read_text()
    errs = check(text)
    for e in errs:
        print("  !! FAIL:", e)
    if errs:
        return 1
    call = "    if (gb1_base_table_fill(dex, src)) return true;   /* zero card access */\n"
    assert call in text
    mut_a = text.replace(call, "").replace("    RomGb1Species sp;\n    if (!gb_create_locate_rom(GB_GEN1)) {",
                                          "    RomGb1Species sp;\n" + call + "    if (!gb_create_locate_rom(GB_GEN1)) {", 1)
    mut_b = text.replace(call, "")
    mut_c = text.replace("if (gb_create_rom_available()) {\n    GbCreateMode mode", "if (1) {\n    GbCreateMode mode")
    for nm, mut, want in (("A", mut_a, "s2"), ("B", mut_b, "s1"), ("C", mut_c, "s4")):
        assert mut != text, f"MUT-{nm} did not change the source"
        got = check(mut)
        if not any(e.startswith(want) for e in got):
            print(f"  !! FAIL: MUT-{nm} was not caught by {want} (got {got})")
            return 1
    print("PASS: scratch arm reads the table before any card access; 3 mutants caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
