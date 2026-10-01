#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_za_half_swap_pin_test.py -- BACKLOG #315 (a refused-rollback "half a swap" is a WARNING, not a success), pins WITH TEETH.

The engine half (jrnapp_step_pair returns JRN_OK named "half a swap" when the second half is refused AND the rollback fails) is
pinned by tests/host_jrn_funnel_test.c (t_swap_pair_shape_and_rollback_failure (g)). THIS file pins the APP layer (pdna_main.c /
pdna_box.c, GBA halves the host cannot compile) as SOURCE TEXT, and proves each pin RED against a named mutant:

  H1  pdna_app.h: AUR_HALF is APPENDED to the aur enum (after AUR_PARTIAL; no existing value moves).
  H2  app_undo_redo maps the name "half a swap": re-derive + imgf_staged, log, return AUR_HALF -- and it is NOT a TORN:
      the branch never sets the PARTIAL latch (imgf_partial_set) and never returns AUR_PARTIAL.
  H3  box_chord_action has an AUR_HALF branch: a refuse dialog (warn), NO snd_ok, returns BCA_CHANGED (the image DID change).
  H4  the success path still toasts with snd_ok (control: the pin is not 'no snd_ok anywhere'); the AUR_PARTIAL branch is intact.
  S1  (#314a) drop_held's SWAP tail names the step: app_step_name("Swap") sits AFTER the last early refusal of the occupied-cell path and BEFORE
      the destination write + mark_dirty (so the one-shot lands on the step that records the swap's older half), exactly once.
  S2  (#314a) jrn_app.c requires that name on the OLDER half of a pair ("Swap") and keeps "Box move" for the newer half.
  H5  every new dialog string is <= 184 px wide (ui_ptext_fit's clamp), measured with source/ui_font.c's own width table.

Run: python3 tests/host_za_half_swap_pin_test.py   (exit 0 = every pin holds on the real tree AND every mutant is caught)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
S = ROOT / "source"
fails: list[str] = []

_TOK = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])\'|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(t: str) -> str:
    return _TOK.sub(lambda m: m.group(0) if m.group(0)[0] in "\"'" else " ", t)


def body(text: str, name: str) -> str | None:
    t = strip_comments(text)
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", t):
        i = m.end()
        depth = 1
        while i < len(t) and depth:
            depth += (t[i] == "(") - (t[i] == ")")
            i += 1
        j = i
        while j < len(t) and t[j] in " \t\r\n":
            j += 1
        if j < len(t) and t[j] == "{":
            depth = 0
            k = j
            while k < len(t):
                depth += (t[k] == "{") - (t[k] == "}")
                k += 1
                if depth == 0:
                    return t[j:k]
    return None


def before(b: str, a: str, c: str) -> bool:
    ia, ic = b.find(a), b.find(c)
    return ia >= 0 and ic >= 0 and ia < ic


def font_w() -> list[int]:
    t = (S / "ui_font.c").read_text()
    m = re.search(r"ui_font_w\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", t, re.S)
    return [int(x) for x in re.findall(r"\b\d+\b", strip_comments(m.group(1)))] if m else []


def pw(s: str, w: list[int]) -> int:
    return sum(w[(ord(c) if 32 <= ord(c) <= 127 else 63) - 32] for c in s)


def pins(src: dict[str, str]) -> list[tuple[str, bool]]:
    m, bx, h = src["pdna_main.c"], src["pdna_box.c"], src["pdna_app.h"]
    out: list[tuple[str, bool]] = []
    en = re.search(r"enum\s*\{\s*AUR_DONE\s*=\s*0\s*,([^}]*)\}", strip_comments(h))
    names = [x.strip() for x in en.group(1).split(",")] if en else []
    out.append(("H1 AUR_HALF is APPENDED after AUR_PARTIAL (existing values unmoved)",
                names[:7] == ["AUR_OFF", "AUR_ARENA", "AUR_NOTHING", "AUR_FLOOR", "AUR_DIVERGED", "AUR_ERR", "AUR_PARTIAL"] and names[7:] == ["AUR_HALF"]))
    ur = body(m, "app_undo_redo") or ""
    i = ur.find('"half a swap"')
    seg = ur[i:ur.find("return AUR_HALF") + 15] if i >= 0 and "return AUR_HALF" in ur else ""
    out.append(("H2a app_undo_redo maps the 'half a swap' name to AUR_HALF", i >= 0 and "return AUR_HALF" in ur))
    out.append(("H2b the branch re-derives and stages BEFORE returning", before(seg, "app_journal_rederive()", "imgf_staged(&g_img)") and bool(seg)))
    out.append(("H2c the branch is NOT a TORN: no PARTIAL latch, no AUR_PARTIAL inside it", bool(seg) and "imgf_partial" not in seg and "AUR_PARTIAL" not in seg))
    ch = body(bx, "box_chord_action") or ""
    bs = ch[ch.find("AUR_HALF"):].split("else if (rc == AUR_ARENA)")[0] if "AUR_HALF" in ch else ""
    out.append(("H3a box_chord_action has an AUR_HALF branch that is a refuse dialog", bool(bs) and "chord_refuse(" in bs))
    out.append(("H3b the AUR_HALF branch plays NO snd_ok and returns BCA_CHANGED", bool(bs) and "snd_ok" not in bs and "return BCA_CHANGED" in bs))
    out.append(("H3c the AUR_HALF branch never touches the PARTIAL latch (it is not a TORN)", bool(bs) and "imgf_partial" not in bs and "AUR_PARTIAL" not in bs))
    out.append(("H4a the AUR_DONE success path still toasts with snd_ok (control)", before(ch, "rc == AUR_DONE", "snd_ok()") and ch.find("snd_ok()") < ch.find("AUR_HALF") if "AUR_HALF" in ch else False))
    out.append(("H4b the AUR_PARTIAL branch is intact beside it", "AUR_PARTIAL" in ch and "PARTIAL STEP" in ch))
    w = font_w()
    strs = re.findall(r'chord_refuse\("(HALF A SWAP)",\s*"([^"]*)",\s*"([^"]*)"\)', bs)
    ok = bool(w) and len(w) >= 96 and bool(strs) and all(pw(x, w) <= 184 for t in strs for x in t)
    out.append(("H5 every new dialog string fits ui_ptext_fit's 184 px clamp" + (" (" + ", ".join(f"{pw(x, w)}" for t in strs for x in t) + " px)" if strs and w else ""), ok))
    db = body(bx, "drop_held") or ""
    anchor = "bc_is_native(recs + (uint32_t)cur * 80) && !bc_is_native(s_held)"
    tail = db[db.find(anchor):] if anchor in db else ""
    i_name = tail.find('app_step_name("Swap")')
    i_dirty = tail.find("src->mark_dirty()")
    i_guard = tail.rfind("snd_deny(); return recs;", 0, i_name if i_name >= 0 else 0)
    out.append(("S1a drop_held's SWAP tail calls app_step_name(\"Swap\") exactly once, before the destination write + mark_dirty",
                db.count('app_step_name("Swap")') == 1 and 0 <= i_name < i_dirty and i_name < tail.find("memcpy(recs + (uint32_t)cur * 80, s_held, 80)")))
    out.append(("S1b ... and after the last early refusal of the occupied path (a refused swap must not leave a rename behind)", i_guard >= 0 and i_guard < i_name))
    ja = src["jrn_app.c"]
    jc = strip_comments(ja)
    out.append(("S2a the older half must be named \"Swap\" (ja_older_eligible), the newer half \"Box move\"",
                re.search(r"ja_older_eligible\(const JrnRec\* r\)\s*\{[^}]*strcmp\(r->name, \"Swap\"\) == 0", jc) is not None
                and re.search(r"ja_newer_eligible\(const JrnRec\* r\)\s*\{[^}]*strcmp\(r->name, \"Box move\"\) == 0", jc) is not None))
    pc = body(ja, "ja_group_at") or ""
    out.append(("S2b ja_group_at judges every older half (pair and chain link) with ja_older_eligible and the newest with ja_newer_eligible",
                "ja_older_eligible(&rn)" in pc and "ja_older_eligible(&rp)" in pc and "ja_newer_eligible(&rm)" in pc))
    sp = body(ja, "jrnapp_step_pair") or ""
    out.append(("T1 (#323) the plain path marks a REDO of the older half ('Swap') as 'Swap (half)' -- redo only (a plain undo of it lands on the whole pre-swap image)",
                bool(re.search(r'rc == JRN_OK && dir > 0 && name && strcmp\(name, "Swap"\) == 0\) memcpy\(name, "Swap \(half\)", 12\)', sp))
                and before(sp, "if (g < 2)", '"Swap (half)"') and before(sp, '"Swap (half)"', "rc = jrnapp_step(dir, n1)")))
    out.append(("T2 (#323) the whole-press name stays plain 'Swap' (control)", 'memcpy(name, "Swap", 5)' in sp))
    w2 = font_w()
    tw = [pw(x, w2) for x in ("Undid: Swap (half)", "Redid: Swap (half)")] if w2 else [999]
    out.append(("T3 (#323) both marked toasts fit 184 px (" + "/".join(str(x) for x in tw) + " px)", all(x <= 184 for x in tw)))
    out.append(("T4 (#323) the chord toast's %.17s keeps the whole marked name (11 chars)", 'redo ? "Redid: %.17s" : "Undid: %.17s"' in bx and len("Swap (half)") <= 17))
    return out


def load() -> dict[str, str]:
    return {n: (S / n).read_text() for n in ("pdna_main.c", "pdna_box.c", "pdna_app.h", "jrn_app.c")}


def mut(src: dict[str, str], f: str, old: str, new: str) -> dict[str, str]:
    assert old in src[f], f"mutant anchor missing in {f}: {old[:70]!r}"
    d = dict(src)
    d[f] = src[f].replace(old, new, 1)
    return d


MUTANTS: list[tuple[str, str, str, str, str]] = [
    ("M1 AUR_HALF inserted mid-enum (values shift)", "pdna_app.h", "AUR_ERR, AUR_PARTIAL", "AUR_ERR, AUR_HALF, AUR_PARTIAL", "H1"),
    ("M2 the half-swap name is no longer mapped", "pdna_main.c", 'strcmp(name, "half a swap") == 0', 'strcmp(name, "half a swapX") == 0', "H2a"),
    ("M3 the half-swap branch sets the PARTIAL latch", "pdna_main.c", "    return AUR_HALF;", "    imgf_partial_set(&g_img);\n    return AUR_HALF;", "H2c"),
    ("M4 the half-swap branch skips the re-derive", "pdna_main.c", "    app_journal_rederive();\n    imgf_staged(&g_img);\n    log_line(\"journal: %s HALF", "    imgf_staged(&g_img);\n    log_line(\"journal: %s HALF", "H2b"),
    ("M5 the chord plays the success tone on a half swap", "pdna_box.c", 'chord_refuse("HALF A SWAP"', 'snd_ok(); chord_refuse("HALF A SWAP"', "H3b"),
    ("M6 the chord's half-swap branch returns BCA_NONE", "pdna_box.c", "return BCA_CHANGED;                                      /* the image DID change, but it is a CONSISTENT half", "return BCA_NONE;                                      /* the image DID change, but it is a CONSISTENT half", "H3b"),
    ("M7 the chord has no AUR_HALF branch", "pdna_box.c", "} else if (rc == AUR_HALF) {", "} else if (rc == 9998) {", "H3a"),
    ("M8 the half-swap branch latches PARTIAL in the chord", "pdna_box.c", 'chord_refuse("HALF A SWAP"', 'imgf_partial_set(&g_img); chord_refuse("HALF A SWAP"', "H3c"),
    ("M12 the older half no longer needs the Swap name", "jrn_app.c", 'strcmp(r->name, "Swap") == 0; }', 'strcmp(r->name, "Box move") == 0; }', "S2a"),
    ("M13 the pair walk judges the older half with the newer rule", "jrn_app.c", "!ja_older_eligible(&rn)", "!ja_newer_eligible(&rn)", "S2b"),
    ("M14 the chain link's older half judged with the newer rule", "jrn_app.c", "!ja_older_eligible(&rp)", "!ja_newer_eligible(&rp)", "S2b"),
    ("M10 the Swap one-shot is dropped from drop_held", "pdna_box.c", '  app_step_name("Swap");', "", "S1a"),
    ("M11 the Swap one-shot is set BEFORE the early refusals", "pdna_box.c", '  if (s_orig_slot >= 0 && s_orig_box != box && src->scope == BOXSCOPE_BANK) { snd_deny(); return recs; }',
     '  app_step_name("Swap");\n  if (s_orig_slot >= 0 && s_orig_box != box && src->scope == BOXSCOPE_BANK) { snd_deny(); return recs; }', "S1"),
    ("M15 the (half) mark is dropped", "jrn_app.c", 'memcpy(name, "Swap (half)", 12)', "(void)0", "T1"),
    ("M16 the mark is applied to undo too", "jrn_app.c", "rc == JRN_OK && dir > 0 && name", "rc == JRN_OK && name", "T1"),
    ("M17 the whole-press name is no longer 'Swap'", "jrn_app.c", 'memcpy(name, "Swap", 5)', 'memcpy(name, "Box move", 9)', "T2"),
    ("M18 the toast truncates the marker", "pdna_box.c", 'redo ? "Redid: %.17s" : "Undid: %.17s"', 'redo ? "Redid: %.8s" : "Undid: %.8s"', "T4"),
    ("M9 a too-wide second line", "pdna_box.c", '"Press again, or check the card."', '"Press again, or check the card and the cart slot."', "H5"),
]


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        fails.append(name)


def main() -> int:
    src = load()
    print("== H1-H5 source-text pins on the REAL tree")
    for n, ok in pins(src):
        check(n, ok)
    print("== mutants: each must turn its named pin RED")
    for name, f, old, new, prefix in MUTANTS:
        try:
            red = [n for n, ok in pins(mut(src, f, old, new)) if not ok]
        except AssertionError as e:
            check(f"mutant {name}", False, str(e))
            continue
        hit = [n for n in red if n.startswith(prefix)]
        check(f"mutant {name} -> RED ({'; '.join(x[:50] for x in hit[:2]) or 'NOT CAUGHT'})", bool(hit))
    print(f"\n{'ALL PINS HOLD' if not fails else 'FAILED: ' + str(len(fails))}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
