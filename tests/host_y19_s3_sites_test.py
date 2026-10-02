#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_y19_s3_sites_test.py -- BACKLOG #234 slice 3: the undo/redo UI's wiring pins, with teeth.

The chord's LOGIC is pure C and pinned behaviourally by tests/host_chord_test.c (mutants M1-M7 there). What
cannot be host-run is the wiring in pdna_box.c / pdna_summary.c / pdna_main.c / jrn_app.c, so it is pinned at
text level (comment-stripped function bodies) and every pin is shown to FAIL on a mutant of the REAL source,
every run:

  P1  pdna_box's key loop feeds chord_frame and wakes on a chord event (`while (!k && !cev)`)
  P2  pdna_summary's key loop feeds chord_frame (SELECT acts on release there too)
  P3  box_chord_action refuses while carrying (s_holding / s_ch_hold / s_item_held) BEFORE it calls the engine
  P4  app_undo_redo refuses the arena loan (T5) BEFORE it calls the engine
  P5  app_undo_redo never goes through the staging funnel (an undo must not record itself) and, on success,
      re-derives the copies and marks the image dirty
  P6  jrnapp_step flushes the pending records before an undo (the one deviation from the brief, on purpose)
  P7  the footer hint and the chord are live only on a Gen-3 PC grid (footer_undo / chord_live)
  P8  the History row exists in the nav table and is dispatched
  P9  a diverged history leaves the box screen (which holds the EWRAM borrow) with code 6 and the home loop
      opens the History screen for it
  P10 pdna_box swallows the spent SELECT hold (`chord_swallow(&chord)`) after the audit, so lifting SELECT is no
      mode-cycle tap

One later pin rides along because it guards the same file at the same text level:

  P11 #347: the summary's icon fallback retires the plan (icon_store_plan(0, 0), artless only) BEFORE the
      icon fetch -- the store-side pin in host_iconstore_test.c cannot see this call site, and deleting it
      regresses #347 with the full host suite still green
  P12 #346b: the progress frame's icon fallback (a mon carried ACROSS boxes by a Bank drop is off-plan) retires
      the plan the same way, BEFORE its icon fetch
  P13 #319: app_discard_staged's PARTIAL-latch clear sits behind the SHORT-READ predicate (`ok && rsz < g_save_size`
      demotes ok) -- sf_read_full returns SF_OK on a short read, so a bare `ok` would clear the latch over a
      card-head + staged-tail chimera
  P14 #353: app_save_finalize's SD arm commits EXACTLY g_save_size bytes -- both sf_write_verified and
      sf_where_are_the_bytes take g_save_size (never the G3_SAVE_FILE_SIZE cap, which would write the previous
      save's bytes as a 64 KiB dump's slot B), and a size RAM gate refuses an impossible size BEFORE the write
  P15 #353: view_save pads a short Gen-3 read with 0xFF (after the Game Boy fork, which keeps its pristine copy
      in the idle upper half), so no foreign trainer's bytes survive in g_save's tail
  P16 #354: view_save warns ONLY on `damaged_fallback || game_loads_other` (snd_error, heartbeat+perf pause, one DAMAGED SAVE
      msg_wait), after the parse log line and before the party read; never on the slot_damaged log-only branch; both app_can_edit
      variants and the read-only why/footer honour game_loads_other
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
fails: list[str] = []


def rd(name: str) -> str:
    return (SRC / name).read_text(errors="replace")


def strip_comments(t: str) -> str:
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def body(text: str, fn: str) -> str:
    """The comment-stripped body of the function definition `fn` (brace matched); '' when absent."""
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


def before(b: str, first: str, second: str) -> bool:
    i, j = b.find(first), b.find(second)
    return i >= 0 and j >= 0 and i < j


def checks(box: str, summ: str, main: str, jrn: str, lay: str, prog: str) -> dict[str, bool]:
    pb = body(box, "pdna_box")
    ps = body(summ, "pdna_summary")
    if not ps:                                   # the summary loop's function name differs; fall back to the whole file
        ps = strip_comments(summ)
    bca = body(box, "box_chord_action")
    aur = body(main, "app_undo_redo")
    jst = body(jrn, "jrnapp_step")
    fu = body(box, "footer_undo")
    ads = body(main, "app_discard_staged")        # #319
    us = body(box, "box_undo_scope")              # #234 s4: the ONE scope predicate (a Gen-3 PC grid or a RESIDENT Game Boy grid)
    asf = body(main, "app_save_finalize")         # #353
    sdarm = asf.split("#else", 1)[1] if "#else" in asf else ""
    vsv = body(main, "view_save")
    return {
        "P1 box loop feeds chord_frame and wakes on the chord": bool(pb) and "chord_frame(&chord" in pb and "while (!k && !cev)" in pb,
        "P2 summary loop feeds chord_frame": "chord_frame(&chord" in ps,
        "P3 carrying is refused before the engine": bool(bca) and before(bca, "s_holding", "app_undo_redo(") and "s_ch_hold" in bca and "s_item_held" in bca,
        "P4 the arena loan is refused before the engine (T5)": bool(aur) and before(aur, "app_arena_held()", "jrnapp_step_pair(") and "imgf_arena_ok" in aur,
        "P5 undo never uses the funnel; re-derives + dirties on success": bool(aur) and "app_stage_sections" not in aur and "img_stage" not in aur
            and "app_journal_rederive()" in aur and "imgf_staged(&g_img)" in aur,
        "P6 pending records are flushed before an undo": bool(jst) and before(jst, "jrn_pending(&s_j)", "jrn_undo("),
        "P7 footer/chord live only on a Gen-3 PC grid or a resident Game Boy grid": bool(fu) and "box_undo_scope(src)" in fu and "app_undo_live()" in fu
            and bool(us) and "BOXSCOPE_PC" in us and "!src->is_bank" in us and "BOXSCOPE_GB" in us and "pdna_gen12_resident()" in us
            and re.search(r"chord_live\s*=\s*box_undo_scope\(src\)\s*&&\s*app_can_edit\(\)", strip_comments(box)) is not None,
        "P8 History row in the nav table + dispatched": "X(NV_HISTORY" in lay and "case NV_HISTORY:" in main,
        "P9 diverged -> return 6 -> home loop opens History": "cr == BCA_HISTORY" in strip_comments(box) and "return 6;" in strip_comments(box)
            and re.search(r"if \(r == 6\)\s*pdna_history_screen\(\);", strip_comments(main)) is not None,
        "P10 the spent SELECT hold is swallowed (no mode-cycle tap)": bool(pb) and "chord_swallow(&chord);" in pb,
        "P11 #347: summary icon fallback retires the plan before the fetch": re.search(
            r"#if !PDNA_MON_ICONS_ART_COMPILED\s*icon_store_plan\(0, 0\);\s*#endif\s*"
            r"ui_sprite\(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_for_form", strip_comments(summ)) is not None,
        "P13 #319: the PARTIAL-latch clear is behind the short-read predicate": bool(ads)
            and re.search(r"if \(ok && rsz < g_save_size\)\s*\{[^}]*ok = false;", ads) is not None
            and re.search(r"if \(ok\)\s*imgf_partial_clear\(&g_img\);", ads) is not None
            and before(ads, "rsz < g_save_size", "imgf_partial_clear(") and ads.count("imgf_partial_clear(") == 1
            and "sf_read_full(g_path, g_save, G3_SAVE_FILE_SIZE, &rsz) == SF_OK" in ads,
        "P14 #353: the SD finalize commits exactly g_save_size bytes behind a size gate": bool(sdarm)
            and "sf_write_verified(g_path, g_save, g_save_size)" in sdarm
            and "sf_where_are_the_bytes(g_path, g_save, g_save_size)" in sdarm
            and re.search(r"g_save_size < \(uint32_t\)G3_SLOT_BYTES \|\| g_save_size > \(uint32_t\)G3_SAVE_FILE_SIZE\)\s*\{[^}]*return false;", sdarm) is not None
            and before(sdarm, "g_save_size > (uint32_t)G3_SAVE_FILE_SIZE", "sf_write_verified(")
            and before(sdarm, "g_save_size > (uint32_t)G3_SAVE_FILE_SIZE", "sf_backup")   # review-zp F1: the RAM gate precedes ALL card I/O
            and sdarm.count("sf_write_verified(") == 1 and sdarm.count("G3_SAVE_FILE_SIZE") == 1,   # review-zp F2: no second full-cap write
        "P15 #353: view_save pads a short Gen-3 read with 0xFF after the Game Boy fork": bool(vsv)
            and re.search(r"if \(!err && sz < \(uint32_t\)G3_SAVE_FILE_SIZE\)\s*\{\s*memset\(g_save \+ sz, 0xFF, \(size_t\)G3_SAVE_FILE_SIZE - sz\);", vsv) is not None
            and before(vsv, "pdna_gen12_show_image(path, g_save, sz", "memset(g_save + sz, 0xFF"),
        "P16 #354: view_save warns on damaged_fallback / game_loads_other behind hb+perf pauses; both app_can_edit variants lock on game_loads_other": bool(vsv)
            and re.search(r"if \(g_vinfo\.damaged_fallback \|\| g_vinfo\.game_loads_other\)\s*\{\s*snd_error\(\);\s*hb_pause\(\);\s*perf_span_pause\(\);\s*"
                          r"if \(g_vinfo\.game_loads_other\)\s*msg_wait\(\"DAMAGED SAVE\", UI_WARN,[^;]*;\s*else\s*msg_wait\(\"DAMAGED SAVE\", UI_WARN,[^;]*;\s*"
                          r"perf_span_resume\(\);\s*hb_resume\(\);\s*\}", vsv) is not None
            and vsv.count("DAMAGED SAVE") == 2 and "gen3_slot_consistent" not in vsv
            and before(vsv, "gen3_parse_into(g_save, sz, &g_vinfo", "g_vinfo.damaged_fallback")
            and before(vsv, "g_vinfo.damaged_fallback", "pk_read_party_auto(")
            and len(re.findall(r"bool app_can_edit\(void\)\s*\{[^}]*g_vinfo\.game_loads_other[^}]*\}", strip_comments(main))) == 2
            and "Game loads damaged copy." in body(main, "app_readonly_why")
            and "damaged save: locked  B back" in body(main, "app_readonly_footer"),
        "P12 #346b: progress-frame icon fallback retires the plan before the fetch": re.search(
            r"#if !PDNA_MON_ICONS_ART_COMPILED\s*icon_store_plan\(0, 0\);\s*#endif\s*"
            r"ui_sprite\(SPR_X \+ \(MON_FRONT_W - MON_ICON_W\) / 2, SPR_Y \+ \(MON_FRONT_H - MON_ICON_H\) / 2,\s*"
            r"MON_ICON_W, MON_ICON_H, egg \? mon_icon_egg\(\) : mon_icon_for_form", strip_comments(prog)) is not None,
    }


def main() -> int:
    box, summ, mn, jrn, lay, prog = (rd("pdna_box.c"), rd("pdna_summary.c"), rd("pdna_main.c"), rd("jrn_app.c"), rd("pdna_layout.h"), rd("pdna_progress.c"))
    print("real tree:")
    real = checks(box, summ, mn, jrn, lay, prog)
    for k, v in real.items():
        print(f"  {'ok  ' if v else 'FAIL'} {k}")
        if not v:
            fails.append(k)

    def mutant(tag: str, which: str, old: str, new: str, expect_red: str) -> None:
        texts = {"box": box, "summ": summ, "main": mn, "jrn": jrn, "lay": lay, "prog": prog}
        if old not in texts[which]:
            print(f"  FAIL {tag}: target not found verbatim (source drifted)")
            fails.append(tag)
            return
        texts[which] = texts[which].replace(old, new, 1)
        r = checks(texts["box"], texts["summ"], texts["main"], texts["jrn"], texts["lay"], texts["prog"])
        red = [k for k, v in r.items() if not v]
        ok = any(k.startswith(expect_red) for k in red)
        print(f"  {'ok  ' if ok else 'FAIL'} {tag}: mutant makes {expect_red} RED (red: {[k.split()[0] for k in red]})")
        if not ok:
            fails.append(tag)

    print("mutants (each must turn its pin RED):")
    mutant("M-P1 loop does not wake on the chord", "box", "while (!k && !cev);", "while (!k);", "P1")
    mutant("M-P2 summary bypasses the chord", "summ", "(void)chord_frame(&chord,", "(void)0; (void)(&chord,", "P2")
    mutant("M-P3 carrying check moved after the engine", "box", "  if (s_holding || s_ch_hold || s_item_held) {", "  rc = app_undo_redo(redo ? 1 : -1, name);\n  if (s_holding || s_ch_hold || s_item_held) {", "P3")
    mutant("M-P4 T5 check dropped", "main", "if (!gb && (app_arena_held() || imgf_arena_ok(&g_img) == false)) return AUR_ARENA;   /* T5: g_pc is a loan / ahead of the image */", "", "P4")
    mutant("M-P5 undo routed through the funnel", "main", "    app_journal_rederive();\n    imgf_staged(&g_img);                          /* the image is ahead of the card: the exit save confirms once */\n    log_line(\"journal: %s '%s'", "    app_stage_sections(0, 13, 0);\n    log_line(\"journal: %s '%s'", "P5")
    mutant("M-P6 flush-before-undo removed", "jrn", "if (dir < 0 && jrn_pending(&s_j)) (void)jrnapp_flush();", "", "P6")
    mutant("M-P7 chord live on every source", "box", "const bool chord_live = box_undo_scope(src) && app_can_edit();", "const bool chord_live = app_can_edit();", "P7")
    mutant("M-P7b the chord goes live on a read-only Game Boy mount", "box", "(src->scope == BOXSCOPE_GB && pdna_gen12_resident())", "(src->scope == BOXSCOPE_GB)", "P7")
    mutant("M-P7c the footer hint ignores the journal state", "box", "return box_undo_scope(src) && app_undo_live();", "return box_undo_scope(src);", "P7")
    mutant("M-P8 History row not dispatched", "main", "case NV_HISTORY: pdna_history_screen(); break;", "", "P8")
    mutant("M-P9 home loop ignores code 6", "main", "if (r == 6) pdna_history_screen();", "", "P9")
    mutant("M-P10 chord_swallow removed", "box", "chord_swallow(&chord);", "", "P10")
    mutant("M-P11 the #347 retire deleted from the call site", "summ", "    icon_store_plan(0, 0);\n", "", "P11")
    mutant("M-P12 the #346b retire deleted from the progress frame", "prog", "    icon_store_plan(0, 0);\n", "", "P12")
    mutant("M-P16a the dialog fires on any damaged slot, not only the fallback", "main", "  if (g_vinfo.damaged_fallback || g_vinfo.game_loads_other) {\n    snd_error();", "  if (g_vinfo.slot_damaged[0] || g_vinfo.slot_damaged[1]) {\n    snd_error();", "P16")
    mutant("M-P16b the fallback dialog silenced", "main", "      msg_wait(\"DAMAGED SAVE\", UI_WARN, \"Newer copy damaged (old bug?)\", \"Opened the intact copy.\");\n", "      (void)0;\n", "P16")
    mutant("M-P16c the warning moved before the parse", "main", "  load_phase_n(2, \"parse slots\");", "  if (g_vinfo.damaged_fallback) { snd_error(); msg_wait(\"DAMAGED SAVE\", UI_WARN, \"x\", \"y\"); }\n  load_phase_n(2, \"parse slots\");", "P16")
    mutant("M-P16d dialog only on the fallback (game_loads_other branch unreachable)", "main", "  if (g_vinfo.damaged_fallback || g_vinfo.game_loads_other) {", "  if (g_vinfo.damaged_fallback) {", "P16")
    mutant("M-P16e game_loads_other dialog silenced", "main", "      msg_wait(\"DAMAGED SAVE\", UI_WARN, \"Game loads the damaged copy.\", \"Intact copy shown, read-only.\");\n", "      (void)0;\n", "P16")
    mutant("M-P16f the delta app_can_edit ignores game_loads_other", "main", "  return !pdna_romcheck_bad() && !(g_vinfo.valid && app_rom_is_hack(g_game)) &&\n         !g_vinfo.game_loads_other;", "  return !pdna_romcheck_bad() && !(g_vinfo.valid && app_rom_is_hack(g_game));", "P16")
    mutant("M-P16g the GBA app_can_edit ignores game_loads_other", "main", " &&\n         !(g_vinfo.valid && g_vinfo.game_loads_other);", ";", "P16")
    mutant("M-P16h heartbeat not paused around the dialog", "main", "    hb_pause(); perf_span_pause();\n", "", "P16")
    mutant("M-P16i the read-only reason ignores the damaged copy", "main", "  if (g_vinfo.valid && g_vinfo.game_loads_other) return \"Game loads damaged copy.\";\n", "", "P16")
    mutant("M-P13a the short-read predicate stripped (bare ok)", "main", "  if (ok && rsz < g_save_size) {", "  if (0) {", "P13")
    mutant("M-P13b the latch-clear no longer gated on ok", "main", "if (ok) imgf_partial_clear(&g_img);", "imgf_partial_clear(&g_img);", "P13")
    mutant("M-P13c the demotion dropped", "main", "    ok = false;\n  }\n#endif\n  if (!ok) log_line(\"discard:", "  }\n#endif\n  if (!ok) log_line(\"discard:", "P13")
    mutant("M-P13d a second, unconditional latch-clear", "main", "  if (ok) jrnapp_after_discard(&g_rec);", "  if (ok) jrnapp_after_discard(&g_rec);\n  imgf_partial_clear(&g_img);", "P13")
    mutant("M-P13e rsz never filled (always refuses: latched until reopen)", "main", "G3_SAVE_FILE_SIZE, &rsz) == SF_OK;", "G3_SAVE_FILE_SIZE, 0) == SF_OK;", "P13")
    mutant("M-P14a the cap restored at the write", "main", "sf_write_verified(g_path, g_save, g_save_size)", "sf_write_verified(g_path, g_save, G3_SAVE_FILE_SIZE)", "P14")
    mutant("M-P14b the cap restored at the where-check", "main", "sf_where_are_the_bytes(g_path, g_save, g_save_size)", "sf_where_are_the_bytes(g_path, g_save, G3_SAVE_FILE_SIZE)", "P14")
    mutant("M-P14c the size gate removed", "main", "if (g_save_size < (uint32_t)G3_SLOT_BYTES || g_save_size > (uint32_t)G3_SAVE_FILE_SIZE) {", "if (0) {", "P14")
    mutant("M-P14d the size gate no longer refuses", "main", "msg_wait(\"SAVE REFUSED\", UI_WARN, \"Bad image size.\", \"NOT written.\");\n    return false;", "msg_wait(\"SAVE REFUSED\", UI_WARN, \"Bad image size.\", \"NOT written.\");", "P14")
    mutant("M-P14e a second full-cap write", "main", "st = sf_write_verified(g_path, g_save, g_save_size);", "st = sf_write_verified(g_path, g_save, g_save_size);\n  st = sf_write_verified(g_path, g_save, G3_SAVE_FILE_SIZE);", "P14")
    mutant("M-P14f a backup issued before the size gate", "main", "  /* #353: write exactly what view_save loaded.", "  (void)sf_backup(g_path, bak, sizeof(bak));\n  /* #353: write exactly what view_save loaded.", "P14")
    mutant("M-P15a the pad removed", "main", "    memset(g_save + sz, 0xFF, (size_t)G3_SAVE_FILE_SIZE - sz);", "", "P15")
    mutant("M-P15b the pad disabled", "main", "  if (!err && sz < (uint32_t)G3_SAVE_FILE_SIZE) {\n    /* g_save-write-ok: loader: pads", "  if (0) {\n    /* g_save-write-ok: loader: pads", "P15")
    if fails:
        print("FAILED:", ", ".join(fails))
        return 1
    print("host_y19_s3_sites_test: all pins hold, every mutant red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
