#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_z9_torn_pin_test.py -- z9 fix pass, D10 ruling 9 (TORN containment), pins WITH TEETH.

The engine reports a chain whose rollback failed as TORN; the APP layer (pdna_main.c / pdna_gen12.c / pdna_box.c, GBA halves
the host cannot compile) must contain it. Containment is four sentences, each pinned below as SOURCE TEXT (this file reads
the real sources, function body by function body, comments stripped), and every pin is proven RED against a named mutant:

  P1  load-time TORN discards at the offer: app_journal_offer's JRN_E_TORN branch re-derives, latches, SAYS (msg_wait) and
      THEN discards (Gen-3 app_discard_staged + re-derive, GB gb_discard_staged); no early return before the discard.
  P2  the PARTIAL latch is set at the chord's and the jump's TORN mapping sites (app_undo_redo / app_history_jump), cleared
      ONLY by a successful discard (app_discard_staged / GB app_gb_discarded) or a fresh load.
  P3  EVERY commit path consults it: app_commit_block (sb1/sb2/pc), app_commit_pc, app_commit_all, app_commit_sb12, the
      app_save_finalize funnel, gb_persist (every GB write incl. "exit"); the exit confirm's A reaches app_commit_pc.
  P4  the chord's TORN is a warning: box_chord_action's AUR_PARTIAL branch is a refuse dialog, NO snd_ok, BCA_CHANGED.
  P5  (pure C) the flag semantics -- imgf_clear never clears the latch; only imgf_partial_clear does -- by rebuilding
      tests/host_jrn_funnel_test.c against mutated copies of img_flags.h.

Run: python3 tests/host_z9_torn_pin_test.py   (exit 0 = every pin holds on the real tree AND every mutant is caught)
"""
from __future__ import annotations

import os
import re
import shutil
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
    """The brace-matched body of the DEFINITION of `name` (first match followed by '{'), comments stripped."""
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
    """Every source-text pin as (name, holds)."""
    m, g, bx = src["pdna_main.c"], src["pdna_gen12.c"], src["pdna_box.c"]
    out: list[tuple[str, bool]] = []
    off = body(m, "app_journal_offer") or ""
    torn = off[off.find("JRN_E_TORN"):off.find("JRN_E_TORN") + 1400] if "JRN_E_TORN" in off else ""
    torn = torn.split("if (k > 0)")[0]
    out.append(("P1a offer TORN branch: rederive -> latch -> msg_wait(PARTIAL RE-APPLY) -> discard, in that order",
                before(torn, "app_journal_rederive()", "imgf_partial_set(&g_img)") and before(torn, "imgf_partial_set(&g_img)", 'msg_wait("PARTIAL RE-APPLY"')
                and before(torn, 'msg_wait("PARTIAL RE-APPLY"', "app_discard_staged()")))
    out.append(("P1b offer TORN branch discards on BOTH sides (Gen-3 app_discard_staged + re-derive, GB gb_discard_staged)",
                "gb_discard_staged()" in torn and re.search(r"app_discard_staged\(\);\s*app_journal_rederive\(\);", torn) is not None))
    out.append(("P1c the dialog says what happened ('Save left as it was.')", 'Save left as it was.' in strip_comments(m) and "return;" in torn and torn.rfind("return;") > torn.find("discard_staged")))
    out.append(("P1d no return between the latch and the discard", "return" not in torn[:torn.find("discard_staged")]))
    ds = body(m, "app_discard_staged") or ""
    out.append(("P2a app_discard_staged clears the latch ONLY on a successful re-read (if (ok) imgf_partial_clear)",
                re.search(r"if\s*\(ok\)\s*imgf_partial_clear\(&g_img\)", ds) is not None))
    ur = body(m, "app_undo_redo") or ""
    out.append(("P2b app_undo_redo: TORN / 'partial step' -> rederive + imgf_staged + latch BEFORE returning AUR_PARTIAL",
                'strcmp(name, "partial step")' in ur and "JRN_E_TORN" in ur
                and before(ur, "app_journal_rederive()", "imgf_partial_set(&g_img)") and before(ur, "imgf_staged(&g_img)", "imgf_partial_set(&g_img)")
                and before(ur, "imgf_partial_set(&g_img)", "return AUR_PARTIAL")))
    hj = body(m, "app_history_jump") or ""
    out.append(("P2c app_history_jump: a TORN jump re-derives (even with n == 0) and latches",
                re.search(r"n\s*>\s*0\s*\|\|\s*rc\s*==\s*JRN_E_TORN", hj) is not None and re.search(r"if\s*\(rc\s*==\s*JRN_E_TORN\)\s*imgf_partial_set\(&g_img\)", hj) is not None))
    out.append(("P2d a fresh load (view_save) and a GB session open/close/discard clear the latch",
                "imgf_partial_clear(&g_img)" in (body(m, "view_save") or "") and "imgf_partial_clear(&g_img)" in (body(m, "app_gb_journal_open") or "")
                and "imgf_partial_clear(&g_img)" in (re.search(r"void app_gb_discarded\(void\)[^\n]*", strip_comments(m)) or [""])[0]))
    out.append(("P1e the offer's discard dispatch has the right polarity (GB session -> gb_discard_staged, else Gen-3)",
                re.search(r"if\s*\(pdna_gen12_resident\(\)\)\s*gb_discard_staged\(\);\s*else\s*\{\s*app_discard_staged\(\);\s*app_journal_rederive\(\);\s*\}", torn) is not None))
    clear_fns = ("app_discard_staged", "app_gb_journal_open", "view_save")
    one_liners = [l for l in strip_comments(m).splitlines() if "imgf_partial_clear(&g_img)" in l]
    out.append(("P2e the latch is cleared at EXACTLY the five ruled sites, nowhere else (a retry / rest point must not unlatch)",
                strip_comments(m).count("imgf_partial_clear(") == 5 and "imgf_partial_clear(" not in strip_comments(g) + strip_comments(bx)
                and all("imgf_partial_clear(&g_img)" in (body(m, f) or "") for f in clear_fns)
                and sum(1 for l in one_liners if re.match(r"\s*void app_gb_(discarded|close)\(void\)", l)) == 2))
    gd = body(g, "gb_discard_staged") or ""
    out.append(("P2f the GB discard unlatches only after a FULL-length re-read (got == len) then app_gb_discarded",
                re.search(r"st\s*!=\s*SF_OK\s*\|\|\s*got\s*!=\s*g_ed->len", gd) is not None and before(gd, "got != g_ed->len", "app_gb_discarded()")))
    helper = body(m, "app_partial_refuse") or ""
    out.append(("P3a app_partial_refuse reads the latch, speaks the partial wording, returns true",
                "imgf_partial(&g_img)" in helper and 'msg_wait("PARTIAL IMAGE"' in helper and helper.count("return true") == 1 and helper.count("return false") == 1))
    for fn in ("app_commit_block", "app_commit_pc", "app_commit_all", "app_commit_sb12"):
        b = body(m, fn) or ""
        refuse_at = b.find("app_partial_refuse()")
        stage_at = min([x for x in (b.find("app_stage_sections"), b.find("app_commit_block"), b.find("app_save_finalize"), b.find("app_arena_release")) if x >= 0] or [10**9])
        out.append((f"P3b {fn} refuses on the latch BEFORE it stages or writes", refuse_at >= 0 and refuse_at < stage_at))
    out.append(("P3c app_commit_sb1 / app_commit_sb2 route through app_commit_block",
                len(re.findall(r"bool app_commit_sb[12]\(void\)\s*\{\s*return app_commit_block\(", strip_comments(m))) == 2))
    fz = body(m, "app_save_finalize") or ""
    out.append(("P3d the app_save_finalize funnel itself refuses a latched image BEFORE any write (defence in depth)",
                before(fz, "imgf_partial(&g_img)", "flashsave_write(") and before(fz, "imgf_partial(&g_img)", "sf_write_verified(")))
    gp = body(g, "gb_persist") or ""
    out.append(("P3e gb_persist (every GB write, 'exit' included) refuses on the latch BEFORE the backup and the write",
                before(gp, "app_partial_refuse()", "sf_backup_rolling(") and before(gp, "app_partial_refuse()", "sf_write_verified(")))
    out.append(("P3f the exit confirm's A reaches the latch: flush_on_exit -> app_xfer_save_now -> app_commit_pc",
                "app_xfer_save_now()" in (body(m, "flush_on_exit") or "") and "app_commit_pc()" in (body(m, "app_xfer_save_now") or "")))
    ch = body(bx, "box_chord_action") or ""
    seg = re.split(r"else if \(rc == AUR_(?:ARENA|HALF)\)", ch[ch.find("AUR_PARTIAL"):])[0] if "AUR_PARTIAL" in ch else ""   # za #315: AUR_HALF's branch follows AUR_PARTIAL's
    out.append(("P4a box_chord_action has an AUR_PARTIAL branch that is a refuse dialog", "AUR_PARTIAL" in ch and "chord_refuse(" in seg))
    out.append(("P4b the AUR_PARTIAL branch plays NO snd_ok and returns BCA_CHANGED", bool(seg) and "snd_ok" not in seg and "return BCA_CHANGED" in seg))
    out.append(("P4c the AUR_DONE success path still toasts with snd_ok (control: the pin is not 'no snd_ok anywhere')",
                before(ch, "rc == AUR_DONE", "snd_ok()") and ch.find("snd_ok()") < ch.find("AUR_PARTIAL")))
    return out


def load() -> dict[str, str]:
    return {n: (S / n).read_text() for n in ("pdna_main.c", "pdna_gen12.c", "pdna_box.c")}


def mut(src: dict[str, str], fname: str, old: str, new: str) -> dict[str, str]:
    assert old in src[fname], f"mutant anchor missing in {fname}: {old[:60]!r}"
    d = dict(src)
    d[fname] = src[fname].replace(old, new, 1)
    return d


DISCARD_LINE = "if (pdna_gen12_resident()) gb_discard_staged(); else { app_discard_staged(); app_journal_rederive(); }"
TEXT_MUTANTS: list[tuple[str, str, str, str, str]] = [
    # (name, file, old, new, the pin prefix that must go RED)
    ("D1 drop the whole discard at the offer", "pdna_main.c", DISCARD_LINE, "", "P1"),
    ("D2 drop only the GB discard at the offer", "pdna_main.c", DISCARD_LINE, "if (!pdna_gen12_resident()) { app_discard_staged(); app_journal_rederive(); }", "P1b"),
    ("D3 drop the Gen-3 re-derive after the discard", "pdna_main.c", DISCARD_LINE, "if (pdna_gen12_resident()) gb_discard_staged(); else { app_discard_staged(); }", "P1b"),
    ("D4 return before the discard", "pdna_main.c", 'msg_wait("PARTIAL RE-APPLY", UI_WARN, "A step was only partly re-applied:", "Save left as it was.");',
     'msg_wait("PARTIAL RE-APPLY", UI_WARN, "A step was only partly re-applied:", "Save left as it was."); return;', "P1d"),
    ("D5 latch AFTER the dialog (a failed discard would not stay latched)", "pdna_main.c",
     "imgf_partial_set(&g_img);                               /* latched until", "/* latched until", "P1a"),
    ("L1 app_commit_block drops its latch consult", "pdna_main.c", "if (app_partial_refuse()) return false;           /* D10/9.2: sb1/sb2/pc all route here */", "", "P3b app_commit_block"),
    ("L2 app_commit_pc drops its latch consult", "pdna_main.c", "bool app_commit_pc(void)  {\n  if (app_partial_refuse()) return false;           /* D10/9.2 */\n", "bool app_commit_pc(void)  {\n", "P3b app_commit_pc"),
    ("L3 app_commit_all drops its latch consult", "pdna_main.c", "  if (app_partial_refuse()) return false;           /* D10/9.2 */\n  /* g_pc is Tier B's donor", "  /* g_pc is Tier B's donor", "P3b app_commit_all"),
    ("L4 app_commit_sb12 drops its latch consult", "pdna_main.c", "bool app_commit_sb12(void) {\n  if (app_partial_refuse()) return false;           /* D10/9.2 */\n", "bool app_commit_sb12(void) {\n", "P3b app_commit_sb12"),
    ("L5 the finalize funnel drops its latch check", "pdna_main.c", "if (imgf_partial(&g_img)) {                       /* D10/9.2 defence in depth", "if (0) {                       /* D10/9.2 defence in depth", "P3d"),
    ("L6 gb_persist drops its latch consult", "pdna_gen12.c", "if (app_partial_refuse()) {   /* D10/9.2", "if (0) {   /* D10/9.2", "P3e"),
    ("L7 the helper stops reading the latch", "pdna_main.c", "if (!imgf_partial(&g_img)) return false;\n  log_line(\"commit refused", "if (1) return false;\n  log_line(\"commit refused", "P3a"),
    ("L8 the chord TORN mapping no longer latches", "pdna_main.c", "    imgf_staged(&g_img);\n    imgf_partial_set(&g_img);\n    log_line(\"journal: %s TORN", "    imgf_staged(&g_img);\n    log_line(\"journal: %s TORN", "P2b"),
    ("L9 the jump TORN mapping no longer latches", "pdna_main.c", "if (rc == JRN_E_TORN) imgf_partial_set(&g_img);", "", "P2c"),
    ("L10 the jump skips the re-derive when n == 0", "pdna_main.c", "if (n > 0 || rc == JRN_E_TORN) {", "if (n > 0) {", "P2c"),
    ("L11 a failed discard re-read still clears the latch", "pdna_main.c", "if (ok) imgf_partial_clear(&g_img); ", "imgf_partial_clear(&g_img); ", "P2a"),
    ("L12 a fresh load keeps a stale latch", "pdna_main.c", "imgf_partial_clear(&g_img);                  /* D10/9.2: a new load", "/* D10/9.2: a new load", "P2d"),
    ("L13 the exit save no longer reaches app_commit_pc", "pdna_main.c", "  if (app_commit_pc()) {\n    /* BACKLOG #383", "  if (1) {\n    /* BACKLOG #383", "P3f"),
    ("F1 restore snd_ok in the chord's TORN branch", "pdna_box.c", 'chord_refuse("PARTIAL STEP", "A step was only partly applied:", "exit without saving it.");',
     'snd_ok(); chord_refuse("PARTIAL STEP", "A step was only partly applied:", "exit without saving it.");', "P4b"),
    ("F2 delete the chord's AUR_PARTIAL branch (it falls to 'Nothing was changed')", "pdna_box.c", "} else if (rc == AUR_PARTIAL) {", "} else if (rc == 9999) {", "P4a"),
    ("F3 the chord's TORN returns BCA_NONE (the grid would not re-fetch)", "pdna_box.c", "return BCA_CHANGED;                                      /* the image DID change", "return BCA_NONE;                                      /* the image DID change", "P4b"),
]


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        fails.append(name)


COMMON = ["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf", "-Dsniprintf=snprintf", "-Dvsniprintf=vsnprintf"]


def funnel_with(flags_text: str, tag: str) -> tuple[int, str]:
    d = Path(tempfile.mkdtemp(prefix="z9mut_"))
    try:
        for n in ("img_stage.h", "img_stage.c", "jrn_app.c", "jrn_app.h"):
            shutil.copy(S / n, d / n)
        (d / "img_flags.h").write_text(flags_text)
        exe = Path(tempfile.gettempdir()) / f"hz9_{tag}_{os.getpid()}"
        srcs = [str(ROOT / "tests/host_jrn_funnel_test.c"), str(d / "img_stage.c"), str(S / "gen3_save.c"), str(S / "journal.c"),
                str(S / "journal_undo.c"), str(S / "journal_fs.c"), str(d / "jrn_app.c"),
                str(ROOT / "lib/fatfs/ff.c"), str(ROOT / "lib/fatfs/ffunicode.c"), str(ROOT / "tests/hostfat/ramdisk.c")]
        inc = ["-I", str(d), "-I", str(ROOT / "tests" / "hostfat"), "-I", str(ROOT / "lib" / "fatfs"), "-I", str(S), "-I", str(ROOT / "tests")]
        b = subprocess.run(COMMON + inc + srcs + ["-o", str(exe)], capture_output=True, text=True, cwd=ROOT)
        if b.returncode != 0:
            return 99, b.stderr[-300:]
        r = subprocess.run([str(exe), *map(str, SAVES)], capture_output=True, text=True, cwd=ROOT)
        exe.unlink(missing_ok=True)
        return r.returncode, r.stdout[-300:]
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main() -> int:
    src = load()
    print("== P1-P4 source-text pins on the REAL tree")
    real = pins(src)
    for n, ok in real:
        check(n, ok)
    print("== text mutants: each must turn its named pin RED")
    for name, f, old, new, prefix in TEXT_MUTANTS:
        try:
            red = [n for n, ok in pins(mut(src, f, old, new)) if not ok]
        except AssertionError as e:
            check(f"mutant {name}", False, str(e))
            continue
        hit = [n for n in red if n.startswith(prefix)]
        check(f"mutant {name} -> RED ({'; '.join(x[:60] for x in hit[:2]) or 'NOT CAUGHT'})", bool(hit))
    print("== P5 pure-C flag semantics: host_jrn_funnel_test.c against mutated img_flags.h")
    real_flags = (S / "img_flags.h").read_text()
    rc, out = funnel_with(real_flags, "ctl")
    check("positive control: the unmutated flags pass the funnel test", rc == 0, out)
    c_mutants = [
        ("C1 imgf_clear also clears the latch (a failed discard would unlatch)", "f->pc_unstaged = false; f->image_dirty = false;", "f->pc_unstaged = false; f->image_dirty = false; f->partial = false;"),
        ("C2 imgf_partial_clear is a no-op (a discard never unlatches)", "static inline void imgf_partial_clear(ImgFlags* f) { if (f) f->partial = false; }", "static inline void imgf_partial_clear(ImgFlags* f) { (void)f; }"),
        ("C3 imgf_partial_set does not latch", "f->partial = true; f->image_dirty = true;", "f->image_dirty = true;"),
        ("C4 imgf_partial_set does not stage (the exit prompt would skip a partial image)", "f->partial = true; f->image_dirty = true;", "f->partial = true;"),
    ]
    for name, old, new in c_mutants:
        assert old in real_flags, old
        rc, out = funnel_with(real_flags.replace(old, new, 1), name[:2])
        check(f"mutant {name} -> funnel test RED", rc != 0, f"rc {rc}")
    print(f"\n{'ALL PINS HOLD' if not fails else 'FAILED: ' + str(len(fails))}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
