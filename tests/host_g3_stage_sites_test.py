#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_g3_stage_sites_test.py -- BACKLOG #234 slice 0: the staging funnel's pins, with teeth.

pdna_main.c is GBA-only, so its wiring is checked at text level (comment-stripped function
bodies); the LOGIC lives in pure C (source/img_flags.h, source/img_stage.c) and is proven
behaviourally by tests/host_imgstage_test.c. Every pin here is shown to FAIL on a mutant, every
run (the mutants are built in a scratch dir; the real tree is never touched):

  C pins (host_imgstage_test.c recompiled against a mutated copy of the pure-C core):
    M1  the OLD fold condition back (fold whenever the PC edit/image is pending)  -> RED
    M2  an exit gate blind to an SB2-only change (PC-edit-only, the old shape)      -> RED
    M3  a box drop that does NOT stage eagerly                                     -> RED
    M4  the arena/SAVE FIRST refusal dropped (unstaged-only gate)                  -> RED
    M5  the funnel that forgets to mark the image dirty                            -> RED
  Wiring pins (pdna_main.c, text level):
    W1  app_save_finalize folds via img_fold_pc BEFORE the checksum pass; no direct section write
    W2  flush_on_exit gates on imgf_exit_prompt and words the prompt via imgf_exit_line
    W3  app_arena_acquire is gated on imgf_arena_ok
    W4  app_mark_pc_dirty stages through img_pc_edited (the eager stage at the drop)
    W5  every commit/stage site routes through app_stage_sections, none writes a section itself
    W6  the Makefile runs check-gsave-writes on every build
  #234 slice 2 (the funnel FEEDS the journal): tests/host_jrn_funnel_test.c, recompiled per mutant
    F1  the recorder never called (a commit stages but records nothing)              -> RED
    F2  the crossed epoch ignored (a cross-file op no longer floors the offer)      -> RED
    F3  scopes do not defer (a drop records two steps, and writes the image early)  -> RED
    F4  old/new swapped in the diff (the record cannot be applied or undone)        -> RED
  Wiring pins (slice 2): W7 get_fattime honours the journal's held stamp; W8 the journal never opens
    without write access; W9 the Gen-3 'Save X?' prompts are gone and the STAY prompts are not; W10
    the load/rest/discard hooks sit where D1/D6 want them; W11 every cross-file op bumps the epoch.
  Grep guard (tools/check_gsave_writes.py): real tree clean; a planted stray write is caught;
  an annotated one is not; a stray inside the funnel itself is allowed.
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
sys.path.insert(0, str(ROOT / "tools"))
import check_gsave_writes as guard  # noqa: E402

MAIN = (ROOT / "source" / "pdna_main.c").read_text(errors="replace")
ROMS = Path(os.environ.get("ROMS", "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"))
SAVES = sorted(ROMS.glob("*.sav")) or sorted((ROOT / "tests" / "fixtures").glob("*.sav"))
fails: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        fails.append(name)


def bodies(text: str) -> dict[str, str]:
    clean = guard.strip_comments(text)
    lines = clean.split("\n")
    return {n: "\n".join(lines[a - 1:b]) for n, a, b in guard.func_ranges(clean)}


# ---------------------------------------------------------------- C mutation harness
def build_and_run(core_dir: Path, tag: str) -> tuple[int, str]:
    exe = Path(tempfile.gettempdir()) / f"himgstage_{tag}_{os.getpid()}"
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(core_dir), "-I", str(ROOT / "source"),
           str(ROOT / "tests" / "host_imgstage_test.c"), str(core_dir / "img_stage.c"),
           str(ROOT / "source" / "gen3_save.c"), str(ROOT / "source" / "journal.c"),
           str(ROOT / "source" / "journal_undo.c"), "-o", str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        return 99, b.stderr[-300:]
    r = subprocess.run([str(exe), *map(str, SAVES)], capture_output=True, text=True)
    exe.unlink(missing_ok=True)
    return r.returncode, r.stdout[-300:]


def build_and_run_funnel(core_dir: Path, tag: str) -> tuple[int, str]:
    exe = Path(tempfile.gettempdir()) / f"hjf_{tag}_{os.getpid()}"
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf",
           "-Dsniprintf=snprintf", "-Dvsniprintf=vsnprintf", "-I", str(core_dir), "-I", str(ROOT / "tests" / "hostfat"),
           "-I", str(ROOT / "lib" / "fatfs"), "-I", str(ROOT / "source"),
           str(ROOT / "tests" / "host_jrn_funnel_test.c"), str(core_dir / "img_stage.c"),
           str(ROOT / "source" / "gen3_save.c"), str(ROOT / "source" / "journal.c"),
           str(ROOT / "source" / "journal_undo.c"), str(ROOT / "source" / "journal_fs.c"),
           str(ROOT / "lib" / "fatfs" / "ff.c"), str(ROOT / "lib" / "fatfs" / "ffunicode.c"),
           str(ROOT / "tests" / "hostfat" / "ramdisk.c"), "-o", str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        return 99, b.stderr[-300:]
    r = subprocess.run([str(exe), *map(str, SAVES)], capture_output=True, text=True)
    exe.unlink(missing_ok=True)
    return r.returncode, r.stdout[-300:]


def funnel_mutant(tag: str, edits: list[tuple[str, str, str]]) -> tuple[int, str]:
    d = Path(tempfile.mkdtemp(prefix="jrnmut_"))
    try:
        for f in ("img_flags.h", "img_stage.h", "img_stage.c"):
            shutil.copy(ROOT / "source" / f, d / f)
        for f, old, new in edits:
            t = (d / f).read_text()
            if old not in t:
                return 98, f"mutation anchor missing in {f}: {old[:50]}"
            (d / f).write_text(t.replace(old, new, 1))
        return build_and_run_funnel(d, tag)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def mutant(tag: str, edits: list[tuple[str, str, str]]) -> tuple[int, str]:
    """edits = (file, old, new); each old must occur, so a refactor cannot silently void a mutant."""
    d = Path(tempfile.mkdtemp(prefix="imgmut_"))
    try:
        for f in ("img_flags.h", "img_stage.h", "img_stage.c"):
            shutil.copy(ROOT / "source" / f, d / f)
        for f, old, new in edits:
            t = (d / f).read_text()
            if old not in t:
                return 98, f"mutation anchor missing in {f}: {old[:50]}"
            (d / f).write_text(t.replace(old, new, 1))
        return build_and_run(d, tag)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def c_pins() -> None:
    rc, out = build_and_run(ROOT / "source", "clean")
    check("C core: the real pure-C funnel passes host_imgstage_test.c", rc == 0, out)
    muts = {
        "M1 old fold condition back": [("img_flags.h",
            "return f ? f->pc_unstaged : false;", "return f ? (f->pc_unstaged || f->image_dirty) : false;")],
        "M2 old exit gate (PC moves only)": [("img_flags.h",
            "return f ? f->image_dirty : false; }\n\n/* Wording", "return f ? f->pc_unstaged : false; }\n\n/* Wording")],
        "M3 drop does not stage eagerly": [("img_stage.c",
            "bool staged = can_stage &&", "bool staged = false && can_stage &&")],
        # #234 s2: pc_moved retired -- M4 flips to the new truth: the arena gate is pc_unstaged ALONE, so a mutant that lets
        # the arena take a g_pc that is AHEAD of g_save (the tileset-bytes-in-every-box case) must go RED.
        "M4 arena lent while g_pc is ahead of g_save": [("img_flags.h",
            "return f ? !f->pc_unstaged : false; }", "return f ? true : false; }")],
        "M5 funnel forgets to mark the image dirty": [("img_stage.c", "  imgf_staged(f);\n", "")],
    }
    for name, edits in muts.items():
        rc, out = mutant(name[:2], edits)
        check(f"C mutant {name} -> RED (rc={rc})", rc == 1, out)
    rc, out = build_and_run_funnel(ROOT / "source", "clean")
    check("C core: the funnel feeds the journal (host_jrn_funnel_test.c) on the real source", rc == 0, out)
    fmuts = {
        "F1 recorder never called": [("img_stage.c", "  rec_step(r, save, slot, mask, blk);\n", "")],
        "F2 crossed epoch ignored": [("img_stage.c", "crossed = r->epoch != r->epoch_seen;", "crossed = 0;")],
        "F3 scopes do not defer": [("img_stage.c", "if (allow_defer && r && r->depth) {", "if (0 && allow_defer && r && r->depth) {")],
        "F4 old/new swapped in the diff": [("img_stage.c", "jrn_step_region(r->j, (uint8_t)id, old_blk, blk[id])",
                                            "jrn_step_region(r->j, (uint8_t)id, blk[id], old_blk)")],
    }
    for name, edits in fmuts.items():
        rc, out = funnel_mutant(name[:2], edits)
        check(f"C mutant {name} -> RED (rc={rc})", rc == 1, out)


# ---------------------------------------------------------------- wiring pins
def wiring(text: str) -> list[str]:
    """Return the list of violated wiring pins for pdna_main.c text (empty = all hold)."""
    b, bad = bodies(text), []
    fin = b.get("app_save_finalize", "")
    if "img_fold_pc(" not in fin or "gen3_write_full_section" in fin or \
       fin.find("img_fold_pc(") > fin.find("gen3_verify_full_checksums("):
        bad.append("W1 finalize fold")
    fe = b.get("flush_on_exit", "")
    if "imgf_exit_prompt(&g_img)" not in fe or "imgf_exit_line(&g_img)" not in fe or "imgf_arena_ok" in fe:
        bad.append("W2 flush_on_exit gate/wording")
    if "imgf_arena_ok(&g_img)" not in b.get("app_arena_acquire", ""):
        bad.append("W3 arena gate")
    mk = b.get("app_mark_pc_dirty", "")
    if "img_pc_edited(" not in mk or "!app_arena_held()" not in mk or "g_vinfo.valid" not in mk:
        bad.append("W4 eager stage at the drop (incl. its can_stage guards)")
    for fn in ("app_commit_block", "app_commit_all", "app_commit_sb12", "app_stage_sb1",
               "app_register_dex_deferred"):
        body = b.get(fn, "")
        if "app_stage_sections(" not in body or "gen3_write_full_section" in body:
            bad.append(f"W5 {fn}")
    return bad


def fbody(text: str, name: str) -> str:
    """The body of function `name` in comment-stripped C, brace-matched (tools/check_gsave_writes.func_ranges
    misses definitions with an __attribute__ in the signature, which several slice-2 hooks have). "" = not found."""
    clean = guard.strip_comments(text)
    m = re.search(r"^[^\n;#]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", clean, re.M)
    if not m:
        return ""
    i, depth = m.end() - 1, 0
    for j in range(i, len(clean)):
        depth += (clean[j] == "{") - (clean[j] == "}")
        if depth == 0:
            return clean[i + 1:j]
    return ""


def slice2_wiring(text: str, disk: str, jrnapp: str) -> list[str]:
    """The #234 slice-2 wiring pins (text level). Empty list = all hold."""
    bad = []
    b = {n: fbody(text, n) for n in ("app_journal_load", "flush_on_exit", "app_discard_staged", "app_xfer_pending_set",
         "app_xfer_promote", "app_pc_release_slot", "app_inject_to_game", "app_bank_defer_delete", "app_xfer_pid_guard",
         "gb_reconcile_release", "xfer_reconcile_apply")}
    gf = fbody(disk, "get_fattime")
    if "jrn_stamp_held()" not in gf or gf.count("jrn_fattime_filter(") < 3:
        bad.append("W7 get_fattime does not honour the held stamp / filter every return")
    jo = fbody(jrnapp, "jrnapp_open")
    if "!write_ok" not in jo or "return JA_OFF" not in jo or jo.find("!write_ok") > jo.find("ja_do_open("):
        bad.append("W8 the journal can open without write access")
    if "app_can_edit()" not in b["app_journal_load"]:
        bad.append("W8 app_journal_load does not pass app_can_edit()")
    for s in ("Save data changes?", "Save bag changes?", "Save Pokedex changes?", "Save Pokeblocks?", "Save events?",
              "Save Secret-Base edits?"):
        for f in sorted((ROOT / "source").glob("*.c")):
            if f.name.startswith("pdna_gb"):
                continue                        # the Gen-1/2 editors keep theirs (not journaled)
            raw = text if f.name == "pdna_main.c" else f.read_text(errors="replace")
            if f'"{s}"' in raw:                 # a quoted literal: a comment that mentions the words cannot trip this
                bad.append(f"W9 removed prompt '{s}' is back in {f.name}")
    for s in ("Release all", "Overwrite", "Delete ALL backups", "PUT IT BACK", "Keep this Pokemon", "SAVE NOW"):
        if not any(s in f.read_text(errors="replace") for f in (ROOT / "source").glob("*.c")):
            bad.append(f"W9 STAY prompt '{s}' vanished")
    if not b["flush_on_exit"].lstrip().startswith("app_journal_rest();"):
        bad.append("W10 flush_on_exit does not start with the rest-point flush")
    if "jrnapp_after_discard(&g_rec)" not in b["app_discard_staged"]:
        bad.append("W10 discard does not re-anchor the journal")
    if not (0 <= text.find("app_journal_load();") < text.find("load_phase_n(11, PDNA_LOAD_PHASE_SIDECARS)")):
        bad.append("W10 the load anchor does not run before the reconcile")
    for fn in ("app_xfer_pending_set", "app_xfer_promote", "app_pc_release_slot", "app_inject_to_game",
               "app_bank_defer_delete", "app_xfer_pid_guard", "gb_reconcile_release", "xfer_reconcile_apply"):
        if not b[fn] or "app_journal_cross()" not in b[fn]:
            bad.append(f"W11 {fn} does not bump the crossed epoch")
    return bad


def wiring_pins() -> None:
    disk = (ROOT / "lib" / "fatfs" / "diskio_write.c").read_text(errors="replace")
    jrnapp = (ROOT / "source" / "jrn_app.c").read_text(errors="replace")
    check("W7-W11 slice-2 wiring holds on the real tree", not slice2_wiring(MAIN, disk, jrnapp), str(slice2_wiring(MAIN, disk, jrnapp)))
    s2muts = {
        "W7 get_fattime reads the RTC while a flush holds the stamp": (MAIN, disk, jrnapp, "disk",
            "if (jrn_stamp_held()) return (DWORD)jrn_fattime_filter(0);\n", ""),
        "W8 the journal opens without write access": (MAIN, disk, jrnapp, "jrnapp",
            "if (!write_ok || !save || !sb2) return JA_OFF;", "if (!save || !sb2) return JA_OFF;"),
        "W9 a removed prompt comes back": (MAIN, disk, jrnapp, "main",
            "static void app_journal_rest(void) {", "static void x_(void) { app_confirm(\"Save Pokeblocks?\", \"\"); }\nstatic void app_journal_rest(void) {"),
        "W10 flush_on_exit loses the rest-point flush": (MAIN, disk, jrnapp, "main",
            "static void flush_on_exit(void) {\n  app_journal_rest();", "static void flush_on_exit(void) {\n  "),
        "W10 discard forgets to re-anchor": (MAIN, disk, jrnapp, "main",
            "if (ok) jrnapp_after_discard(&g_rec);", ""),
        "W11 a cross-file op forgets the epoch": (MAIN, disk, jrnapp, "main",
            "void app_bank_defer_delete(int box, int slot, const uint8_t* rec80) { app_journal_cross(); ",
            "void app_bank_defer_delete(int box, int slot, const uint8_t* rec80) { "),
    }
    for name, (m, d, j, which, old, new) in s2muts.items():
        src = {"main": m, "disk": d, "jrnapp": j}[which]
        if old not in src:
            check(f"slice-2 wiring mutant anchor present: {name}", False)
            continue
        mm, dd, jj = m, d, j
        if which == "main": mm = m.replace(old, new, 1)
        elif which == "disk": dd = d.replace(old, new, 1)
        else: jj = j.replace(old, new, 1)
        check(f"slice-2 wiring mutant '{name}' -> RED", bool(slice2_wiring(mm, dd, jj)))


def wiring_pins_slice01() -> None:
    check("W1-W5 wiring holds on the real pdna_main.c", not wiring(MAIN), str(wiring(MAIN)))
    muts = {
        "flush_on_exit gate back to a PC-only reader": ("imgf_exit_prompt(&g_img)) {", "imgf_arena_ok(&g_img)) {"),
        "drop no longer stages": ("img_pc_edited(&g_img, &g_rec, g_save, g_vinfo.slot, g_pc, can_stage);",
                                  "imgf_pc_edited(&g_img, false);"),
        "commit_sb12 writes a section itself": ("bool app_commit_sb12(void) {\n  app_stage_sections(0, 0, g_sb2);",
                                  "bool app_commit_sb12(void) {\n  gen3_write_full_section(g_save, g_vinfo.slot, 0, g_sb2);"),
        "arena gate dropped": ("if (!imgf_arena_ok(&g_img)) return NULL;", ""),
    }
    for name, (old, new) in muts.items():
        if old not in MAIN:
            check(f"wiring mutant anchor present: {name}", False)
            continue
        check(f"wiring mutant '{name}' -> RED", bool(wiring(MAIN.replace(old, new, 1))))
    mk = (ROOT / "Makefile").read_text()
    check("W6 Makefile runs check-gsave-writes before every build", "$(BUILD): check-gsave-writes" in mk)


# ---------------------------------------------------------------- grep guard
def guard_pins() -> None:
    real = guard.scan(MAIN)
    check("guard: the real pdna_main.c is clean", not real, str(real[:3]))
    others = [(p.name, guard.scan(p.read_text(errors="replace"))) for p in sorted((ROOT / "source").glob("*.c"))
              if p.name != "pdna_main.c"]
    check("guard: no other source file writes g_save", all(not v for _, v in others), str([x for x in others if x[1]][:2]))
    stray = MAIN + "\nstatic void stray(void) { g_save[5] = 1; }\n"
    check("guard: a planted `g_save[x] = y;` goes RED", len(guard.scan(stray)) == 1)
    for label, line in (("memcpy", "memcpy(g_save, x, 4);"),
                        ("gen3_write_full_section", "gen3_write_full_section(g_save, 0, 1, d);"),
                        ("|=", "g_save[3] |= 4;"),
                        ("sf_read_full", "sf_read_full(p, g_save, 9, &n);"),
                        ("img_stage_sections", "img_stage_sections(&g_img, &g_rec, g_save, 0, 1, 1, d);"),
                        ("img_scope_close", "img_scope_close(&g_img, &g_rec, g_save, 0);")):
        check(f"guard: a planted {label} write goes RED", len(guard.scan(f"void x(void) {{\n  {line}\n}}\n")) == 1)
    check("guard: an annotated exemption passes",
          not guard.scan("void x(void) {\n  /* g_save-write-ok: loader */\n  memcpy(g_save, a, 4);\n}\n"))
    check("guard: a write inside app_stage_sections passes",
          not guard.scan("static void app_stage_sections(int a) {\n  memcpy(g_save, b, 4);\n}\n"))
    check("guard: a comment mentioning a write does not trip it",
          not guard.scan("void x(void) {\n  /* memcpy(g_save, a, 4); */\n  // g_save[1] = 2;\n}\n"))
    check("guard: reads and == compares do not trip it",
          not guard.scan("void x(void) {\n  int v = g_save[3]; if (g_save[4] == 5) v++;\n  memcpy(a, g_save, 4);\n}\n"))


def main() -> int:
    if not SAVES:
        print("SKIP (no save corpus or fixtures): the C mutation pins need a Gen-3 .sav")
        return 0
    c_pins()
    wiring_pins()
    wiring_pins_slice01()
    guard_pins()
    print()
    if fails:
        print(f"host_g3_stage_sites_test: {len(fails)} FAILED: " + "; ".join(fails))
        return 1
    print("host_g3_stage_sites_test: all pins hold and every mutant is caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
