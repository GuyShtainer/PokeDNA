#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_native_menu_test.py -- BACKLOG #271: a banked Gen-1/2 record (native "GBC1" cell)
must offer the actions that are not Gen-3-only: LEGALITY, DUPLICATE, EXPORT .pk.

pdna_main.c / pdna_box.c do not compile on the host, so this is a text-structural check on
the REAL sources (same style as host_escape_gate_sites_test.py) with a mutation harness.

  (m1) the `if (native) { ... }` row builder in app_mon_menu offers A_LEGAL, A_DUP, A_EXPORT
       (menu-visibility assertion for a banked record);
  (m2) it does NOT offer A_PASTE/A_COPY/A_ITEM (would run Gen-3 bodies on GBC1 bytes); A_TOGAME is
       offered ONLY as the gated row (#271/y10: xg_togame_row -- its native action is a request the
       Bank grid runs through the drop's own arm, pinned in host_y10_togame_sites_test.py);
  (m3) A_LEGAL on a native cell shows the cell's own view and never the Gen-3 box sweep;
  (m4) A_EXPORT on a native cell calls gb_export_native (never pdna_pk_export's .pk3 body);
  (m5) both DUPLICATE sites in pdna_box.c run dup_restamp_native() right after copying into s_held,
       re-fetch `recs` (the serial scan re-pages the shared Bank buffer) and carry the copy ONLY when
       the restamp succeeded (a failed restamp must not set s_holding);
  (m6) dup_restamp_native resyncs the counter to the stored high-water mark BEFORE allocating, refuses
       serial <= stored_max, and delegates the re-pack to the pure bc_restamp_copy (whose ident32-change,
       COPY flag and ledger-bit clearing are host-tested in host_bankcell_test.c).
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
    dup_sites = re.findall(r"memcpy\(s_held, recs \+ \(uint32_t\)(?:gcur|cur) \* 80, 80\);\s*"
                           r"bool dup_ok = dup_restamp_native\(s_held\);\s*"
                           r"recs = src->records\(box\);[^\n]*\n\s*"
                           r"if \(!dup_ok\) \{[^\n]*\n\s*snd_deny\(\);(?: need_full = true;)?\s*\} else \{\s*s_holding = true;", box)
    helper = func(box, "dup_restamp_native(uint8_t held[80]) {")
    return {
        "m1": all(a in nb for a in ("act[n++]=A_LEGAL", "act[n++]=A_DUP", "act[n++]=A_EXPORT")),
        "m2": (not any(a in nb for a in ("A_PASTE", "A_COPY", "A_ITEM"))
               and all("xg_togame_row(" in ln for ln in nb.split("\n") if "A_TOGAME" in ln)),
        "m3": legal,
        "m4": export,
        "m5": len(dup_sites) == 2,
        "m6": (0 <= helper.find("bank_serial_max(") < helper.find("pdna_bank_serial_resync(stored_max)")
               < helper.find("pdna_bank_next_serial()") < helper.find("serial <= stored_max")
               < helper.find("bc_restamp_copy(held, serial)") and "serial == 0" in helper),
    }


def main() -> int:
    menu = func(strip((ROOT / "pdna_main.c").read_text(errors="replace")),
                "bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank,")
    box = strip((ROOT / "pdna_box.c").read_text(errors="replace"))
    ok = analyse(menu, box)
    for k, v in ok.items():
        check(v, f"{k} does not hold on the real source")
    muts = {
        "m1": lambda m, b: (m.replace("lab[n]=PDNA_LBL_EXPORT_PK; act[n++]=A_EXPORT;", "", 1), b),
        "m2": lambda m, b: (m.replace("lab[n]=PDNA_LBL_DUPLICATE; act[n++]=A_DUP;\n    lab[n]=PDNA_LBL_EXPORT_PK", "lab[n]=PDNA_LBL_DUPLICATE; act[n++]=A_DUP; act[n++]=A_PASTE;\n    lab[n]=PDNA_LBL_EXPORT_PK", 1), b),
        "m2b": lambda m, b: (m.replace("if (is_bank && xg_togame_row(is_bank, app_gen3_pc_live(), g_have_pc)) { lab[n]=PDNA_LBL_TO_GAME;", "{ lab[n]=PDNA_LBL_TO_GAME;", 1), b),
        "m3": lambda m, b: (m.replace("if (native) { pdna_legality_show(&m0); return false; }", "", 1), b),
        "m4": lambda m, b: (m.replace("if (native) { (void)gb_export_native(rec); return false; }", "", 1), b),
        "m5": lambda m, b: (m, b.replace("bool dup_ok = dup_restamp_native(s_held);", "bool dup_ok = true;", 1)),
        "m5b": lambda m, b: (m, re.sub(r"(dup_restamp_native\(s_held\);\s*)recs = src->records\(box\);", r"\1", b, count=1)),
        "m5c": lambda m, b: (m, re.sub(r"(if \(!dup_ok\) \{[^\n]*\n\s*snd_deny\(\);)", r"\1 s_holding = true;", b, count=1)),
        "m6": lambda m, b: (m, b.replace("(void)pdna_bank_serial_resync(stored_max);\n", "", 1)),
        "m6b": lambda m, b: (m, b.replace("serial == 0 || serial <= stored_max", "serial == 0", 1)),
    }
    for k, f in muts.items():
        mm, bb = f(menu, box)
        caught = not analyse(mm, bb)[k[:2]]
        check(caught, f"MUT {k} was NOT caught")
        print(f"  MUT {k}:", "caught" if caught else "MISSED")
    print(f"{checks} checks, {len(fails)} failed")
    for f in fails:
        print("  !! FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
