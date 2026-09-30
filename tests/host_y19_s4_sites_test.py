#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_y19_s4_sites_test.py -- BACKLOG #234 slice 4 (a Game Boy save on the same journal): the wiring pins, with teeth.

Two halves, both shown to FAIL on a mutant of the REAL source every run:

  C half   tests/host_jrn_gb_test.c (the real jrn_app.c / img_stage.c / gb_jkey.c / journal_fs.c over the real FatFs on a
           RAM disk), recompiled against mutated copies of those sources:
             G1 the key ignores the trainer ID            G2 the key drops the Game Boy marker (Gen-1 == Gen-2 key)
             G3 write_ok no longer gates the GB open      G4 the flat accessor's set() patches nothing (undo/re-apply dead)
             G5 the flat accessor's get() reads zeros     G6 a flat resync hashes the NEW image, not the baseline
             G7 the retention cap is not passed to the open   G8 Clear history stops after 3 slot files
             G9 Clear history forgets the directory's own unlink (the old ring's reopen still works; the dir stays)
             G10 img_rec_flat never records
  Text half  (comment-stripped function bodies of pdna_gen12.c / pdna_main.c / the GB screens):
             S1 gb_hold_commit records, re-baselines and writes NOTHING          S2 gb_persist's verified-write pipeline keeps its
             order (backup, write, THEN the step is recorded, then saved)         S3 the six retired prompts are skipped only while
             the journal records (gb_hold_live)                                   S4 the STAY list: exactly the transfer walls keep
             gb_persist, every plain edit holds                                   S5 the exit: flush + ONE confirm, discard re-reads
             S6 the session opens its journal after g_ed is latched and closes it at exit   S7 the grid loop handles code 6 + flushes
             S8 Settings: the two rows, Clear behind the Omega gate + a destructive confirm; the only f_unlink in the journal is
             jrnfs_clear_key, reached from jrnapp_clear only            S9 the cap survives config.cfg (mask keeps bits 24-25)
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
SRC = ROOT / "source"
fails: list[str] = []
n_checks = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global n_checks
    n_checks += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"  [{detail}]"))
    if not ok:
        fails.append(name)


def rd(name: str) -> str:
    return (SRC / name).read_text(errors="replace")


def strip_comments(t: str) -> str:
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def body(text: str, fn: str) -> str:
    t = strip_comments(text)
    m = re.search(r"(?m)^[A-Za-z_][^;{}\n]*\b" + re.escape(fn) + r"\s*\([^;{]*\)\s*\{", t)
    if not m:
        return ""
    i = m.end() - 1
    d = 0
    for j in range(i, len(t)):
        if t[j] == "{":
            d += 1
        elif t[j] == "}":
            d -= 1
            if d == 0:
                return t[i:j + 1]
    return ""


def order(b: str, *needles: str) -> bool:
    pos = [b.find(n) for n in needles]
    return all(p >= 0 for p in pos) and pos == sorted(pos) and len(set(pos)) == len(pos)


# ------------------------------------------------------------------------------------------------ C half
CORE = ("img_flags.h", "img_stage.h", "img_stage.c", "jrn_app.h", "jrn_app.c", "gb_jkey.h", "gb_jkey.c", "journal_fs.h",
        "journal_fs.c")


def build_and_run(core: Path, tag: str) -> tuple[int, str]:
    exe = Path(tempfile.gettempdir()) / f"hjgb_{tag}_{os.getpid()}"
    src = lambda n: str(core / n) if n in CORE else str(SRC / n)       # noqa: E731
    files = ("img_stage.c gb_jkey.c gb_trainer.c gb_fields.c gb_session.c gb_edit.c gen1_save.c gen1_write.c gen2_save.c "
             "gen2_write.c data_tables.c item_map_g2g3.c item_map_g1g2.c gb_item_names.c gb_bag.c gen3_to_gb.c gb_sidecar.c "
             "bank_cell.c gen3_edit.c gen3_mon.c gen3_box.c gen3_save.c gen3_daycare.c journal.c journal_undo.c journal_fs.c "
             "jrn_app.c").split()
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf",
           "-Dsniprintf=snprintf", "-Dvsniprintf=vsnprintf", "-I", str(core), "-I", str(ROOT / "tests" / "hostfat"),
           "-I", str(ROOT / "lib" / "fatfs"), "-I", str(SRC), "-I", str(ROOT / "tests"),
           str(ROOT / "tests" / "host_jrn_gb_test.c"), str(ROOT / "tests" / "gen12_fixture.c")] + [src(f) for f in files] + [
           str(ROOT / "lib" / "fatfs" / "ff.c"), str(ROOT / "lib" / "fatfs" / "ffunicode.c"),
           str(ROOT / "tests" / "hostfat" / "ramdisk.c"), "-o", str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        return 99, b.stderr[-400:]
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    exe.unlink(missing_ok=True)
    return r.returncode, r.stdout[-400:]


def c_mutant(tag: str, edits: list[tuple[str, str, str]]) -> tuple[int, str]:
    d = Path(tempfile.mkdtemp(prefix="gbmut_"))
    try:
        for f in CORE:
            shutil.copy(SRC / f, d / f)
        for f, old, new in edits:
            t = (d / f).read_text()
            if old not in t:
                return 98, f"mutation anchor missing in {f}: {old[:60]}"
            (d / f).write_text(t.replace(old, new, 1))
        return build_and_run(d, tag)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def c_half() -> None:
    rc, out = build_and_run(SRC, "real")
    check("C: the real GB journal core passes host_jrn_gb_test.c", rc == 0, out)
    muts = {
        "G1 the key ignores the trainer ID": [("gb_jkey.c", "tid = (uint16_t)(((uint16_t)s->img[ioff] << 8) | s->img[ioff + 1u]);", "tid = 0;")],
        "G2 the key drops the Game Boy marker": [("gb_jkey.c", "(uint16_t)(GBJ_SID_MARK + (unsigned)g)", "(uint16_t)0")],
        "G3 write_ok no longer gates the GB open": [("jrn_app.c", "if (!write_ok || !img) return JA_OFF;", "if (!img) return JA_OFF;")],
        "G4 the flat accessor's set patches nothing": [("jrn_app.c", "    if (!sec || !src) return -1;\n    memcpy(sec, src, n);\n    return 0;",
                                                         "    if (!sec || !src) return -1;\n    return 0;")],
        "G5 the flat accessor's get reads zeros": [("jrn_app.c", "    if (!sec || !dst) return -1;\n    memcpy(dst, sec, n);",
                                                     "    if (!sec || !dst) return -1;\n    memset(dst, 0, n);")],
        "G6 a flat resync hashes the new image": [("img_stage.c", "memcpy(dst, fp->old_img + (uint32_t)region * fp->regsz + off, n);",
                                                    "memcpy(dst, fp->new_img + (uint32_t)region * fp->regsz + off, n);")],
        "G7 the retention cap is not passed": [("jrn_app.c", "  s_j.max_segs = cap;                                       /* ja_do_open reads it",
                                                 "  /* cap dropped */                                       /* ja_do_open reads it")],
        "G8 Clear history stops after 3 slot files": [("journal_fs.c", "for (s = 1; s <= JRN_RING_MAX; s++) {                           /* bounded",
                                                        "for (s = 1; s <= 3u; s++) {                           /* bounded")],
        "G9 Clear history forgets the directory": [("journal_fs.c", "(void)f_unlink(dir); ", "(void)0; ")],
        "G10 img_rec_flat never records": [("img_stage.c", "  rec_step(r, 0, 0, 0, 0, &fp);\n", "")],
    }
    for name, edits in muts.items():
        rc, out = c_mutant(name[:3].strip(), edits)
        check(f"C mutant {name} -> RED (rc={rc})", rc == 1, out)


# ---------------------------------------------------------------------------------------------- text half
GB_SCREENS = ["pdna_gbbag.c", "pdna_gbclock.c", "pdna_gbdaycare.c", "pdna_gbdex.c", "pdna_gbflags.c", "pdna_gbfly.c",
              "pdna_gbhof.c", "pdna_gbmap.c", "pdna_gbpack.c", "pdna_gbtrainer.c", "pdna_gbedit.c", "pdna_gen12.c"]
STAY = {"xferup", "xferdown", "bank-down", "paste", "exit"}
HOLD = {"boxname", "move", "release", "release-all", "dup", "daycare-put", "create", "bag", "gbclock reset", "gbclock clear",
        "gbclock shift", "daycare-take", "daycare-egg", "daycare-edit", "gb1 teleport", "gb1 teleport undo", "gbflags", "fly",
        "trainer", "dex", "hof edit", "hof clear", "hof setcount", "hof add", "hof delete"}


def tags(texts: dict[str, str], fn: str) -> set[str]:
    out: set[str] = set()
    for t in texts.values():
        for m in re.finditer(r"\b" + fn + r'\(\s*"([^"]+)"\s*\)', strip_comments(t)):
            out.add(m.group(1))
    return out


def pins(T: dict[str, str]) -> dict[str, bool]:
    g12 = T["pdna_gen12.c"]
    main = T["pdna_main.c"]
    hc = body(g12, "gb_hold_commit")
    gp = body(g12, "gb_persist")
    fo = body(g12, "gb_flush_on_exit")
    ds = body(g12, "gb_discard_staged")
    so = body(g12, "pdna_gen12_show_image")
    sc = body(g12, "gb_session_core")
    st = body(main, "pdna_settings")
    jc = body(T["jrn_app.c"], "jrnapp_clear")
    return {
        "S1 gb_hold_commit records, re-baselines and writes nothing":
            bool(hc) and order(hc, "app_gb_stage(g_ed->pristine, g_ed->img", "memcpy(g_ed->pristine, g_ed->img")
            and "sf_write_verified" not in hc and "sf_backup" not in hc and hc.count("gb_persist(") == 2
            and hc.find("gb_persist(") < hc.find("app_gb_stage(")
            and re.search(r"if\s*\(\s*!app_can_edit\(\)\s*\|\|\s*!app_gb_hold_live\(\)\s*\)\s*return gb_persist", hc) is not None
            and re.search(r"if\s*\(\s*!app_gb_stage\([^;]*\)\s*\)\s*return gb_persist", hc) is not None,
        "S11 hold_live is true only while the journal records (JA_OK); an unrecordable step writes at once":
            re.search(r"return\s+app_can_edit\(\)\s*&&\s*st\s*==\s*JA_OK\s*;", body(main, "app_gb_hold_live")) is not None
            and "JA_GAP" not in body(main, "app_gb_hold_live")
            and "jrnapp_state(&g_rec) == JA_OK && g_rec.lost == lost0" in body(main, "app_gb_stage"),
        "S2 gb_persist: backup, verified write, THEN the step, THEN saved, THEN the re-baseline":
            bool(gp) and gp.rfind("app_gb_stage(g_ed->pristine, g_ed->img") > gp.rfind("sf_write_verified(g_ed->path, g_ed->img")
            > gp.rfind("sf_backup_rolling(") > 0 and gp.rfind("app_gb_saved();") > gp.rfind("app_gb_stage(")
            and gp.rfind("memcpy(g_ed->pristine, g_ed->img, g_ed->len);") > gp.rfind("app_gb_saved();")
            and gp.rfind("app_gb_stage(") > gp.rfind("sf_write_verified(") > gp.rfind("sf_backup_rolling("),
        "S3 every retired prompt is skipped only while the journal records":
            all(re.search(r"gb_hold_live\(\)[^\n]*app_confirm\(\"" + re.escape(p) + r"|app_confirm\(\"" + re.escape(p) + r"[^\n]*", strip_comments(T[f]))
                and ("gb_hold_live()" in strip_comments(T[f]))
                for f, p in (("pdna_gbdex.c", "Save Pokedex changes?"), ("pdna_gbbag.c", "Save bag changes?"),
                             ("pdna_gbpack.c", "Save pack changes?"), ("pdna_gbfly.c", "Save fly destinations?"),
                             ("pdna_gbflags.c", "Save data changes?"), ("pdna_gbtrainer.c", "Save trainer changes?")))
            and all(re.search(r"gb_hold_live\(\)\s*(\|\||&&)[^\n]*app_confirm\(\"" + re.escape(p), strip_comments(T[f]))
                    or re.search(r"!gb_hold_live\(\)\s*&&\s*!app_confirm\(\"" + re.escape(p), strip_comments(T[f]))
                    for f, p in (("pdna_gbdex.c", "Save Pokedex changes?"), ("pdna_gbbag.c", "Save bag changes?"),
                                 ("pdna_gbpack.c", "Save pack changes?"), ("pdna_gbfly.c", "Save fly destinations?"),
                                 ("pdna_gbflags.c", "Save data changes?"), ("pdna_gbtrainer.c", "Save trainer changes?"))),
        "S3b the plain legal mon edit asks nothing while the journal records (gbedit_confirm)":
            "gb_hold_live()" in body(T["pdna_gbedit.c"], "gbedit_confirm") and "gb_check(e, &iss)" in body(T["pdna_gbedit.c"], "gbedit_confirm"),
        "S3c the GB quiet confirm needs the summary's quiet posture; the three resident summaries set it":
            "gb_hold_live() && pdna_summary_quiet()" in body(T["pdna_gbedit.c"], "gbedit_confirm")
            and re.search(r"pdna_summary_quiet_save\(true\);\s*nav = pdna_gbsummary\(&edited[^;]*;\s*pdna_summary_quiet_save\(false\);", strip_comments(T["pdna_gbdaycare.c"])) is not None
            and len(re.findall(r"pdna_summary_quiet_save\(true\);\s*(?:int nav = |nav = )?pdna_gbsummary\(&(?:e|box_mon)\b[^;]*;\s*pdna_summary_quiet_save\(false\);", strip_comments(T["pdna_gen12.c"]))) == 2,
        "S4 the STAY list is exactly the transfer walls; every plain edit holds":
            tags(T, "gb_persist") == STAY and tags(T, "gb_hold_commit") == HOLD,
        "S5 the exit: pending records first, ONE confirm, discard re-reads the card":
            bool(fo) and order(fo, "app_gb_rest();", 'app_confirm("Save changes?"', 'gb_persist("exit")', "gb_discard_staged();")
            and "sf_read_full(g_ed->path" in ds and "app_gb_discarded();" in ds,
        "S6 the session opens the journal once g_ed is latched and closes it at exit":
            bool(so) and order(so, "g_ed = ed;", "gb_journal_session_open();")
            and order(so, "gb_session_core(m, 0);", "gb_flush_on_exit();", "app_gb_close();")
            and so.rfind("g_ed = 0;") > so.find("app_gb_close();"),
        "S7 the grid loop: rest point when the grid is left, code 6 opens the History":
            bool(sc) and "app_gb_rest();" in sc and "if (r == 6) pdna_gen12_history();" in sc,
        "S8 Settings: Clear history behind the Omega gate and a destructive confirm":
            bool(st) and order(st, "sel == S_HCLEAR", "cart_writable()", "app_history_bound()", "app_confirm(PDNA_SET_HCLEAR_TITLE", "app_history_clear()")
            and "S_HIST" in st and "HIST_CAP_SHIFT" in st,
        "S8b the ONLY f_unlink in the journal is jrnfs_clear_key, reached from jrnapp_clear alone":
            strip_comments(T["journal.c"]).count("f_unlink") == 0 and strip_comments(T["journal_undo.c"]).count("f_unlink") == 0
            and strip_comments(T["jrn_app.c"]).count("f_unlink") == 0
            and strip_comments(T["journal_fs.c"]).count("f_unlink(") == 2 and body(T["journal_fs.c"], "jrnfs_clear_key").count("f_unlink(") == 2
            and "jrnfs_clear_key(" in jc
            and sum(strip_comments(t).count("jrnfs_clear_key(") for n, t in T.items() if n not in ("journal_fs.c",)) == 1
            and sum(strip_comments(t).count("jrnapp_clear(") for n, t in T.items() if n not in ("jrn_app.c",)) == 1,
        "S9 the retention cap survives config.cfg (the anim mask keeps bits 24-25)":
            "| (3u << HIST_CAP_SHIFT)" in strip_comments(main).replace("(3u<<HIST_CAP_SHIFT)", "(3u << HIST_CAP_SHIFT)") and "app_history_cap()" in body(main, "app_gb_journal_open"),
        "S10 a GB identity edit is a crossed step (trainer name / ID)":
            order(body(T["pdna_gbtrainer.c"], "pdna_gbtrainer"), "const bool ident", "if (ident) app_journal_cross();", 'gb_hold_commit("trainer")'),
    }


def text_half() -> None:
    names = set(GB_SCREENS) | {"pdna_main.c", "jrn_app.c", "journal.c", "journal_undo.c", "journal_fs.c"}
    T = {n: rd(n) for n in names}
    red = [k for k, v in pins(T).items() if not v]
    check("Text: every slice-4 wiring pin holds on the real tree", not red, str(red))

    def mut(tag: str, fn: str, old: str, new: str, expect: str) -> None:
        if old not in T[fn]:
            check(f"Text mutant anchor present: {tag}", False, f"{fn}: {old[:60]}")
            return
        T2 = dict(T)
        T2[fn] = T[fn].replace(old, new, 1)
        r = [k for k, v in pins(T2).items() if not v]
        check(f"Text mutant {tag} -> {expect} RED (red: {[k.split()[0] for k in r]})", any(k.startswith(expect) for k in r))

    mut("S1 hold_commit writes through", "pdna_gen12.c", "  snd_ok();\n  return true;\n}\n\n/* B at the exit confirm",
        "  snd_ok();\n  return gb_persist(what_for_log);\n}\n\n/* B at the exit confirm", "S1")
    mut("S1b hold_commit forgets to record", "pdna_gen12.c", "  if (!app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), false)) return gb_persist(what_for_log);   /* not recorded: no net under a hold -> write now */\n  memcpy",
        "  memcpy", "S1")
    mut("S1c an unrecordable step is held, not written", "pdna_gen12.c", "  if (!app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), false)) return gb_persist(what_for_log);   /* not recorded: no net under a hold -> write now */\n",
        "  (void)app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), false);\n", "S1")
    mut("S11 hold_live true at JA_GAP again", "pdna_main.c", "return app_can_edit() && st == JA_OK;", "return app_can_edit() && (st == JA_OK || st == JA_GAP);", "S11")
    mut("S11b the stage ignores a lost record", "pdna_main.c", "jrnapp_state(&g_rec) == JA_OK && g_rec.lost == lost0", "jrnapp_state(&g_rec) == JA_OK", "S11")
    mut("S2 the step is no longer recorded after the verified write", "pdna_gen12.c",
        "  if (strcmp(what_for_log, \"exit\") != 0 && app_gb_hold_live())\n    (void)app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), true);\n  app_gb_saved();",
        "  app_gb_saved();", "S2")
    mut("S3 the dex prompt is back unconditionally", "pdna_gbdex.c", "if (!gb_hold_live() && !app_confirm(\"Save Pokedex changes?\"", "if (!app_confirm(\"Save Pokedex changes?\"", "S3")
    mut("S3 the bag prompt is back unconditionally", "pdna_gbbag.c", "if (gb_hold_live() || app_confirm(\"Save bag changes?\"", "if (app_confirm(\"Save bag changes?\"", "S3")
    mut("S3b the plain edit still asks", "pdna_gbedit.c", "  if (gb_hold_live() && pdna_summary_quiet()) {\n    GbIssues iss;", "  if (0) {\n    GbIssues iss;", "S3b")
    mut("S3c the GB quiet confirm skips without the summary posture", "pdna_gbedit.c", "gb_hold_live() && pdna_summary_quiet()", "gb_hold_live()", "S3c")
    mut("S3c the create summary forgets to set quiet", "pdna_gen12.c", "  pdna_summary_quiet_save(true);            /* #234 s4: a resident-session edit is held */\n", "", "S3c")
    mut("S4 a transfer wall holds instead of writing", "pdna_gen12.c", '  return gb_persist("xferup");', '  return gb_hold_commit("xferup");', "S4")
    mut("S4b a plain edit writes at once", "pdna_gen12.c", '  return gb_hold_commit("move");', '  return gb_persist("move");', "S4")
    mut("S5 the exit forgets the pending records", "pdna_gen12.c", "  if (!g_ed) return;\n  app_gb_rest();\n  if (!app_gb_dirty()) return;", "  if (!g_ed) return;\n  if (!app_gb_dirty()) return;", "S5")
    mut("S5b B no longer re-reads the card", "pdna_gen12.c", "sf_read_full(g_ed->path, g_ed->img, g_ed->len, &got)", "SF_OK; (void)got", "S5")
    mut("S6 the journal opens before g_ed is latched", "pdna_gen12.c", "      g_ed = ed;\n      gb_journal_session_open();", "      gb_journal_session_open();\n      g_ed = ed;", "S6")
    mut("S6b the recorder is never unbound", "pdna_gen12.c", "{ gb_flush_on_exit(); app_gb_close(); }", "{ gb_flush_on_exit(); }", "S6")
    mut("S7 the grid loop ignores code 6", "pdna_gen12.c", "    else if (r == 6) pdna_gen12_history();", "", "S7")
    mut("S8 Clear history without the Omega gate", "pdna_main.c", "        if (!cart_writable()) { snd_deny(); msg_wait(\"READ-ONLY\", UI_WARN, \"Needs EZ-Flash Omega.\", 0); continue; }\n        if (!app_history_bound())",
        "        if (!app_history_bound())", "S8")
    mut("S8 Clear history without the confirm", "pdna_main.c", "if (app_confirm(PDNA_SET_HCLEAR_TITLE, PDNA_SET_HCLEAR_L1)) {", "if (1) {", "S8")
    mut("S9 the cap bits are dropped on load", "pdna_main.c", "| (3u << HIST_CAP_SHIFT));", ");", "S9")
    mut("S10 an identity edit is not crossed", "pdna_gbtrainer.c", "  if (ident) app_journal_cross();\n", "", "S10")


def main() -> int:
    print("C half (the real core over the RAM-disk FatFs, then mutants):")
    c_half()
    print("text half (wiring pins, then mutants):")
    text_half()
    if fails:
        print("FAILED:", ", ".join(fails))
        return 1
    print(f"host_y19_s4_sites_test: {n_checks} checks, all pins hold and every mutant is red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
