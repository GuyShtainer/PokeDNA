#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_native_menu_test.py -- BACKLOG #271: a banked Gen-1/2 record (native "GBC1" cell)
must offer the actions that are not Gen-3-only: LEGALITY, DUPLICATE, EXPORT .pk.

pdna_main.c / pdna_box.c do not compile on the host, so this is a text-structural check on
the REAL sources (same style as host_escape_gate_sites_test.py) with a mutation harness.

  (m1) the `if (native) { ... }` row builder in app_mon_menu offers A_LEGAL, A_DUP, A_EXPORT
       (menu-visibility assertion for a banked record);
  (m2) it does NOT offer A_TOGAME/A_PASTE/A_COPY/A_ITEM (would run Gen-3 bodies on GBC1 bytes);
  (m3) A_LEGAL on a native cell shows the cell's own view and never the Gen-3 box sweep;
  (m4) A_EXPORT on a native cell calls gb_export_native (never pdna_pk_export's .pk3 body);
  (m5) both DUPLICATE sites in pdna_box.c run dup_restamp_native() right after copying into s_held;
  (m6) dup_restamp_native allocates a fresh serial, sets BC_FLAG_COPY and clears the ledger bits.
"""
from __future__ import annotations
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "source"
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


def analyse(menu: str, box: str) -> dict[str, bool]:
    st = menu.index("int n = 0;")
    nb = menu[menu.index("if (native) {", st):menu.index("} else if (occupied) {")]
    legal = re.search(r"case A_LEGAL:\s*if \(native\) \{ pdna_legality_show\(&m0\);", menu) is not None
    export = re.search(r"case A_EXPORT:\s*if \(native\) \{ \(void\)gb_export_native\(rec\);", menu) is not None
    dup_sites = re.findall(r"memcpy\(s_held, recs \+ \(uint32_t\)(?:gcur|cur) \* 80, 80\);\s*if \(!dup_restamp_native\(s_held\)\)", box)
    helper = func(box, "dup_restamp_native(uint8_t held[80]) {")
    return {
        "m1": all(a in nb for a in ("act[n++]=A_LEGAL", "act[n++]=A_DUP", "act[n++]=A_EXPORT")),
        "m2": not any(a in nb for a in ("A_TOGAME", "A_PASTE", "A_COPY", "A_ITEM")),
        "m3": legal,
        "m4": export,
        "m5": len(dup_sites) == 2,
        "m6": ("pdna_bank_next_serial()" in helper and "BC_FLAG_COPY" in helper and
               "~(BC_FLAG_HAS_XFER_REC | BC_FLAG_QUEUED_PC)" in helper and "serial == 0" in helper),
    }


def main() -> int:
    menu = func(strip((ROOT / "pdna_main.c").read_text(errors="replace")),
                "bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank,")
    box = strip((ROOT / "pdna_box.c").read_text(errors="replace"))
    ok = analyse(menu, box)
    for k, v in ok.items():
        check(v, f"{k} does not hold on the real source")
    muts = {
        "m1": lambda m, b: (m.replace("lab[n]=PDNA_LBL_EXPORT_PK; act[n++]=A_EXPORT;\n    lab[n]=PDNA_LBL_RELEASE", "lab[n]=PDNA_LBL_RELEASE", 1), b),
        "m2": lambda m, b: (m.replace("lab[n]=PDNA_LBL_DUPLICATE; act[n++]=A_DUP;\n    lab[n]=PDNA_LBL_EXPORT_PK", "lab[n]=PDNA_LBL_DUPLICATE; act[n++]=A_DUP; act[n++]=A_TOGAME;\n    lab[n]=PDNA_LBL_EXPORT_PK", 1), b),
        "m3": lambda m, b: (m.replace("if (native) { pdna_legality_show(&m0); return false; }", "", 1), b),
        "m4": lambda m, b: (m.replace("if (native) { (void)gb_export_native(rec); return false; }", "", 1), b),
        "m5": lambda m, b: (m, b.replace("if (!dup_restamp_native(s_held))", "if (false)", 1)),
        "m6": lambda m, b: (m, b.replace("BC_FLAG_COPY", "0", 1) if False else b.replace("| BC_FLAG_COPY)", ")", 1)),
    }
    for k, f in muts.items():
        mm, bb = f(menu, box)
        caught = not analyse(mm, bb)[k]
        check(caught, f"MUT {k} was NOT caught")
        print(f"  MUT {k}:", "caught" if caught else "MISSED")
    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print("  !! FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
