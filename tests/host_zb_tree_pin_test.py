#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_zb_tree_pin_test.py -- BACKLOG #304 (the History TREE, display only), pins WITH TEETH.

The data half (jrnapp_history_tree: summary rows, opened siblings, the 48-row window, the counts) is pinned at RUN time by
tests/host_jrn_funnel_test.c (t_tree_*); the walk itself by tests/host_jrn_kids_test.c. THIS file pins what the host cannot
compile -- the GBA screen source/pdna_hist.c -- as SOURCE TEXT, and proves each pin (and the funnel's tree checks) RED against a named mutant:

  U1  the screen builds its rows with jrnapp_history_tree (the tree), fed the open-fork set.
  U2  A on a SIBLING row (kind == JH_SIB) is DENIED: it sets the one-line footer note and never reaches app_history_jump; and the sibling branch sits
      BEFORE the jump branch (a sibling can never fall through into a jump).
  U3  h_wait takes the deny flag and plays snd_deny (never snd_ok) for A on a sibling row; the screen passes `on_sib` computed from kind == JH_SIB.
  U4  A on a FORK summary row TOGGLES that fork's open flag (open_forks keyed by rows[sel].parent -- the fork point), refetches, and never jumps.
  U5  the jump target is unchanged: A on a step jumps to rows[sel].seq (the root pseudo row to 0), START to rows[0].seq.
  U6  the footer strings (hint per row kind + the denial note) fit ui_text's fixed 8x8 grid: <= 30 characters.
  U7  the summary row renders the count it was given (nsib) and the open/closed glyph; the sibling row shows FLOOR and SAVED marks.
  T1..  (run time) mutants of source/jrn_app.c: the summary miscounts; the branch child counts as its own sibling; an opened fork lists the WRONG
        fork's rows; the window overruns its budget; a sibling is not set off from the branch.  Each must turn host_jrn_funnel_test RED by NAME.

Run: python3 tests/host_zb_tree_pin_test.py   (exit 0 = every pin holds on the real tree AND every mutant is caught)
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
S = ROOT / "source"
ROMS = Path(os.environ.get("ROMS", "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"))
SAVES = sorted(ROMS.glob("*.sav")) or sorted((ROOT / "tests" / "fixtures").glob("*.sav"))
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


def pins(src: dict[str, str]) -> list[tuple[str, bool]]:
    h = src["pdna_hist.c"]
    out: list[tuple[str, bool]] = []
    scr = body(h, "pdna_history_screen_rows") or ""
    hc = strip_comments(h)
    out.append(("U1 the screen builds its rows with jrnapp_history_tree over the open-fork set",
                "jrnapp_history_tree(rows, max, &more, &floor_hit, open_forks, nopen)" in scr and "jrnapp_history(" not in scr))
    sib = re.search(r"else if \(\(k & KEY_A\) && on_sib\)\s*\{([^}]*)\}", scr)
    sibb = sib.group(1) if sib else ""
    out.append(("U2a A on a sibling row sets the denial note and BREAKS to a repaint (no jump)",
                bool(sib) and "note = \"Other branch - not jumpable.\"" in sibb and "break" in sibb and "app_history_jump" not in sibb))
    i_sib = scr.find("(k & KEY_A) && on_sib")
    i_jump = scr.find("app_history_jump(")
    out.append(("U2b the sibling deny branch comes BEFORE the jump (a sibling never falls through into a jump)", 0 <= i_sib < i_jump and scr.count("app_history_jump(") == 1))
    out.append(("U3a h_wait(mask, deny_a) plays snd_deny for A when denied and snd_ok otherwise",
                re.search(r"if \(fresh & KEY_A\) \{ if \(deny_a\) snd_deny\(\); else snd_ok\(\); \}", hc) is not None))
    out.append(("U3b the screen computes on_sib from kind == JH_SIB and hands it to h_wait",
                "bool on_sib = sel < n && rows[sel].kind == JH_SIB;" in scr and "h_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START, on_sib)" in scr))
    tog = re.search(r"else if \(\(k & KEY_A\) && sel < n && rows\[sel\]\.kind == JH_FORK\)\s*\{(.*?)\n      \}", scr, re.S)
    togb = tog.group(1) if tog else ""
    out.append(("U4a A on a fork summary toggles open_forks keyed by the FORK POINT (rows[sel].parent), then refetches",
                bool(tog) and "uint32_t fp = rows[sel].parent;" in togb and "open_forks[nopen++] = fp;" in togb and "refetch = true;" in togb))
    out.append(("U4b ... and the toggle branch never jumps and removes an open fork on the second A",
                bool(tog) and "app_history_jump" not in togb
                and "if (at >= 0) { for (int q = at; q + 1 < nopen; q++) open_forks[q] = open_forks[q + 1]; nopen--; }" in togb and "open_forks[q] == fp) at = q;" in togb))
    out.append(("U4c the toggle branch precedes the jump branch", 0 <= scr.find("kind == JH_FORK") < i_jump))
    out.append(("U5 the jump target: START -> rows[0].seq, A -> rows[sel].seq on a step (0 on the root pseudo row)",
                "uint32_t target = (k & KEY_START) ? rows[0].seq : (sel < n ? rows[sel].seq : 0u);" in scr))
    strs = re.findall(r'return (?:rows\[sel\]\.open \? )?"([^"]*)"(?: : "([^"]*)")?;', body(h, "h_hint") or "")
    flat = [x for t in strs for x in t if x] + ["Other branch - not jumpable."]
    out.append(("U6 every footer string fits ui_text's 8x8 grid (<= 30 chars): " + ", ".join(str(len(x)) for x in flat), bool(strs) and all(len(x) <= 30 for x in flat)))
    row = body(h, "h_row") or ""
    out.append(("U7a the summary row prints the count it was given and an open/closed glyph",
                """r->open ? '-' : '+', (unsigned)r->nsib, r->disc ? PDNA_HIST_DISC : "other", r->nsib == 1 ? "" : "es");""" in row and "r->kind == JH_FORK" in row))
    out.append(("U7b the sibling row shows FLOOR and SAVED, is set off (bar + dim) and says 'other branch'",
                "r->kind == JH_SIB" in row and "r->crossed" in row and '"SAVED" : (r->disc ? PDNA_HIST_DISC : "other branch")' in row and 'ui_ptext(15, y, ink, "|")' in row))
    return out


def load() -> dict[str, str]:
    return {n: (S / n).read_text() for n in ("pdna_hist.c", "jrn_app.c")}


def mut(src: dict[str, str], f: str, old: str, new: str) -> dict[str, str]:
    assert old in src[f], f"mutant anchor missing in {f}: {old[:70]!r}"
    d = dict(src)
    d[f] = src[f].replace(old, new, 1)
    return d


UI_MUTANTS: list[tuple[str, str, str, str, str]] = [
    ("M1 the screen builds the plain branch (no tree)", "pdna_hist.c", "n = jrnapp_history_tree(rows, max, &more, &floor_hit, open_forks, nopen);", "n = jrnapp_history(rows, max, &more, &floor_hit);", "U1"),
    ("M2 the sibling row becomes JUMPABLE (the deny branch jumps)", "pdna_hist.c",
     "else if ((k & KEY_A) && on_sib) { note = \"Other branch - not jumpable.\"; break; }", "else if ((k & KEY_A) && on_sib) { note = \"Other branch - not jumpable.\"; app_history_jump(rows[sel].seq, 0, 0); break; }", "U2a"),
    ("M3 the sibling deny branch is gone (a sibling falls into the jump)", "pdna_hist.c",
     "else if ((k & KEY_A) && on_sib) { note = \"Other branch - not jumpable.\"; break; }", "", "U2a"),
    ("M4 the denied A still plays the CONFIRM blip", "pdna_hist.c", "if (deny_a) snd_deny(); else snd_ok();", "snd_ok();", "U3a"),
    ("M5 on_sib is never computed (always false)", "pdna_hist.c", "bool on_sib = sel < n && rows[sel].kind == JH_SIB;", "bool on_sib = false;", "U3b"),
    ("M6 the fork toggle keys on the wrong field (the step seq, 0 on a summary)", "pdna_hist.c", "uint32_t fp = rows[sel].parent;", "uint32_t fp = rows[sel].seq;", "U4a"),
    ("M7 the fork toggle never closes (no removal)", "pdna_hist.c", "if (at >= 0) { for (int q = at; q + 1 < nopen; q++) open_forks[q] = open_forks[q + 1]; nopen--; }", "if (at >= 0) { }", "U4b"),
    ("M8 the fork toggle does not refetch (the rows would stay stale)", "pdna_hist.c", "          open_forks[nopen++] = fp;\n        }\n        refetch = true;", "          open_forks[nopen++] = fp;\n        }", "U4a"),
    ("M9 START jumps to the selected row", "pdna_hist.c", "(k & KEY_START) ? rows[0].seq :", "(k & KEY_START) ? rows[sel].seq :", "U5"),
    ("M10 the sibling hint overflows the 30-char footer", "pdna_hist.c", '"A -  START newest  B back"', '"A -  START newest  B back, other"', "U6"),
    ("M11 the summary prints one more than it was given", "pdna_hist.c", "(unsigned)r->nsib, r->disc ?", "(unsigned)r->nsib + 1u, r->disc ?", "U7a"),
    ("M12 the sibling row drops its SAVED/other-branch mark", "pdna_hist.c", 'r->saved ? "SAVED" : (r->disc ? PDNA_HIST_DISC : "other branch")', '"recorded"', "U7b"),
]

# (name, edits, the RED line that must appear) -- mutants of source/jrn_app.c run against the REAL funnel test
APP_MUTANTS: list[tuple[str, list[tuple[str, str]], str]] = [
    ("T1 the summary miscounts (+1)", [("f->kind = JH_FORK; f->parent = par[i]; f->nsib = cnt[i];", "f->kind = JH_FORK; f->parent = par[i]; f->nsib = (uint16_t)(cnt[i] + 1u);")], "FORK-2: the summary names the fork point"),
    ("T2 the branch child counts as its own sibling (skip dropped)", [("for (i = 0; i < nb; i++) { par[i] = rows[i].parent; skip[i] = rows[i].seq; }", "for (i = 0; i < nb; i++) { par[i] = rows[i].parent; skip[i] = 0; }")], "FORK-2: the summary names the fork point"),
    ("T3 an opened fork lists the WRONG fork's rows (the first fork's siblings under every fork)", [("jrn_kid_list(&s_j, rows[i].parent, rows[i - 1].seq,", "jrn_kid_list(&s_j, rows[1].parent, rows[0].seq,")], "TWO opened forks, each with ITS OWN sibling"),
    ("T4 the window overruns its budget (trim off by one)", [("while (total > max && nb > 1) {", "while (total > max + 1 && nb > 1) {")], "never over the budget"),
    ("T5 the branch's marks move when a fork is opened (a sibling row is a JH_STEP)", [("sb->kind = JH_SIB; sb->parent = par[i];", "sb->kind = JH_STEP; sb->parent = par[i];")], "OPEN: NEW, summary(open), one sibling"),
    ("T6 a fork directly above a SAVED branch row loses the mark below it (saved cleared on the row after a summary)", [("    if (dest != i) memmove(&rows[dest], &rows[i], sizeof rows[0]);", "    if (dest != i) { memmove(&rows[dest], &rows[i], sizeof rows[0]); rows[dest].saved = 0; }")], "a fork directly above a SAVED mark"),
    ("T7 the sibling's SAVED mark is never set", [("sb->saved = (kid[k].seq == s_saved) ? 1 : 0;", "sb->saved = 0;")], "a sibling that IS the saved step shows SAVED"),
    ("T8 the open set is ignored (an opened fork stays collapsed)", [("static int ja_is_open(const uint32_t* open_forks, int nopen, uint32_t p) {\n  int i;", "static int ja_is_open(const uint32_t* open_forks, int nopen, uint32_t p) {\n  int i;\n  if (p != 0xFFFFFFFFu) return 0;")], "OPEN: NEW, summary(open), one sibling"),
]


def check(label: str, ok: bool) -> None:
    print(("ok   " if ok else "FAIL ") + label)
    if not ok:
        fails.append(label)


def funnel(core: Path) -> tuple[int, str]:
    exe = Path(tempfile.gettempdir()) / f"hjf_zbtree_{os.getpid()}"
    cmd = ["cc", "-std=c11", "-w", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf", "-Dsniprintf=snprintf", "-Dvsniprintf=vsnprintf",
           "-I", str(core), "-I", str(ROOT / "tests" / "hostfat"), "-I", str(ROOT / "lib" / "fatfs"), "-I", str(S),
           str(ROOT / "tests" / "host_jrn_funnel_test.c"), str(S / "img_stage.c"), str(S / "gen3_save.c"), str(S / "journal.c"),
           str(S / "journal_undo.c"), str(S / "journal_fs.c"), str(core / "jrn_app.c"),
           str(ROOT / "lib" / "fatfs" / "ff.c"), str(ROOT / "lib" / "fatfs" / "ffunicode.c"), str(ROOT / "tests" / "hostfat" / "ramdisk.c"), "-o", str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        return 99, b.stderr[-300:]
    r = subprocess.run([str(exe), str(SAVES[0])], capture_output=True, text=True)
    exe.unlink(missing_ok=True)
    return r.returncode, r.stdout


def main() -> int:
    src = load()
    for label, ok in pins(src):
        check(label, ok)
    for name, f, old, new, pin in UI_MUTANTS:
        m = pins(mut(src, f, old, new))
        red = [lb for lb, ok in m if not ok]
        check(f"{name} -> RED by {pin}: {red[0][:60] if red else '(SURVIVED)'}", any(lb.startswith(pin) for lb in red))
    if not SAVES:
        print("SKIP (no corpus .sav: the funnel mutants need one)")
        return 1 if fails else 0
    with tempfile.TemporaryDirectory(prefix="zbtree-") as td:
        core = Path(td)
        rc, out = funnel(S)
        check("T0 the real funnel (tree checks included) is green", rc == 0)
        for name, edits, want in APP_MUTANTS:
            text = (S / "jrn_app.c").read_text()
            for old, new in edits:
                assert text.count(old) == 1, f"mutant anchor missing/duplicated: {old[:70]!r}"
                text = text.replace(old, new, 1)
            (core / "jrn_app.c").write_text(text)
            rc, out = funnel(core)
            line = next((ln for ln in out.splitlines() if want in ln and ln.startswith("FAIL")), "")
            check(f"{name} -> RED: {line[:90] if line else '(rc %d, no matching FAIL line)' % rc}", rc != 0 and bool(line))
    print("FAILED: " + "; ".join(fails) if fails else "host_zb_tree_pin_test: every pin holds and every mutant is caught")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
