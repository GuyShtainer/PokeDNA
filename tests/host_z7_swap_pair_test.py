#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_z7_swap_pair_test.py -- BACKLOG #303 (atomic swap-pair chords) + #308 (GB journal key entropy), pins WITH TEETH.

The behaviour lives in pure C and is proven by tests/host_jrn_funnel_test.c (the swap legs) and tests/host_jrn_gb_test.c
(t_key_entropy, t_old_key_compat). THIS file rebuilds those two tests against MUTATED copies of source/jrn_app.c and
source/gb_jkey.c and requires every mutant to FAIL (a positive control builds the unmutated copy and requires a pass), so no
pin can be decoration. The real tree is never touched (scratch dir).

  #303 mutants (host_jrn_funnel_test.c):
    A1  the pair predicate never fires (ja_chord_pair returns 0)                          -> RED
    A2  the 'exactly one REPLACED slot' rule relaxed (a plain clear + re-add now pairs)    -> RED
    A3  a crossed record no longer disqualifies a half (floor between the halves ignored)  -> RED
    A4  a byte mismatch no longer refutes the pair (a different mon pairs)                 -> RED
    A5  the minimum compared-byte count dropped to 1 (a sparse half pairs)                 -> RED
    A6  no rollback when the second half is refused (a half-swap is left behind)           -> RED
    A7  the section stride is wrong (a section-straddling slot mis-maps)                   -> RED
    A8  History never labels the halves                                                    -> RED
    A9  redo never pairs                                                                   -> RED
  #308 mutants (host_jrn_gb_test.c):
    B1  gb_jkey stops at the first 0xFF again (no escape)                                  -> RED
    B2  the compat open never looks for the old-key journal                                -> RED
    B3  the compat open adopts the old journal WITHOUT the anchor check                    -> RED
    B4  the escaped-name flag (gender) dropped: '9' collides with its own escape bytes     -> RED
    B5  the redirect new -> old is never written                                           -> RED
  za #314/#315 mutants (host_jrn_funnel_test.c): C1..C8 chained swaps, R1 the R1 mislabel (older half no longer needs the "Swap" name),
    H1..H3 the 3-step rollback-failure contract (t_swap_chain_rollback_sweep)
    A10 the Game Boy guard dropped (a swap-shaped GB pair would pair)                      -> RED
  zc #322 mutants R1w..R7w (the walk-fault refusal at each of its seven places); zc #321 mutants (t_swap_seq_adjacency[_chain]): Q1..Q3 each of the three adjacency tests dropped (group first link, group second link, History link);
    Q4a/Q4b/Q4c the seq-gap test off by one in each direction (a real pair stops grouping / a one-step gap pairs), Q4d the History gap off by one
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROMS = Path(os.environ.get("ROMS", "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"))
SAVES = sorted(ROMS.glob("*.sav")) or sorted((ROOT / "tests" / "fixtures").glob("*.sav"))
fails: list[str] = []
S = ROOT / "source"
FILES = ("img_flags.h", "img_stage.h", "img_stage.c", "jrn_app.c", "jrn_app.h", "gb_jkey.c", "gb_jkey.h")


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        fails.append(name)


COMMON = ["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf",
          "-Dsniprintf=snprintf", "-Dvsniprintf=vsnprintf"]


def build_run(core: Path, which: str, tag: str) -> tuple[int, str]:
    exe = Path(tempfile.gettempdir()) / f"hz7_{which}_{tag}_{os.getpid()}"
    inc = ["-I", str(core), "-I", str(ROOT / "tests" / "hostfat"), "-I", str(ROOT / "lib" / "fatfs"), "-I", str(S), "-I", str(ROOT / "tests")]
    tail = [str(ROOT / "lib/fatfs/ff.c"), str(ROOT / "lib/fatfs/ffunicode.c"), str(ROOT / "tests/hostfat/ramdisk.c"), "-o", str(exe)]
    if which == "funnel":
        srcs = [str(ROOT / "tests/host_jrn_funnel_test.c"), str(core / "img_stage.c"), str(S / "gen3_save.c"), str(S / "journal.c"),
                str(S / "journal_undo.c"), str(S / "journal_fs.c"), str(core / "jrn_app.c")]
        args = [str(p) for p in SAVES]
    else:
        names = ("gb_trainer gb_fields gb_session gb_edit gen1_save gen1_write gen2_save gen2_write data_tables item_map_g2g3 "
                 "item_map_g1g2 gb_item_names gb_bag gen3_to_gb gb_sidecar bank_cell gen3_edit gen3_mon gen3_box gen3_save "
                 "gen3_daycare journal journal_undo journal_fs").split()
        srcs = [str(ROOT / "tests/host_jrn_gb_test.c"), str(ROOT / "tests/gen12_fixture.c"), str(core / "img_stage.c"),
                str(core / "gb_jkey.c"), str(core / "jrn_app.c")] + [str(S / f"{n}.c") for n in names]
        args = []
    b = subprocess.run(COMMON + inc + srcs + tail, capture_output=True, text=True, cwd=ROOT)
    if b.returncode != 0:
        return 99, b.stderr[-400:]
    r = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=ROOT)
    exe.unlink(missing_ok=True)
    return r.returncode, r.stdout[-400:]


def mutant(which: str, tag: str, edits: list[tuple[str, str, str]]) -> tuple[int, str]:
    d = Path(tempfile.mkdtemp(prefix="z7mut_"))
    try:
        for f in FILES:
            shutil.copy(S / f, d / f)
        for f, old, new in edits:
            t = (d / f).read_text()
            if old not in t:
                return 98, f"mutation anchor missing in {f}: {old[:60]}"
            (d / f).write_text(t.replace(old, new, 1))
        return build_run(d, which, tag)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def must_fail(which: str, tag: str, label: str, edits: list[tuple[str, str, str]]) -> None:
    rc, out = mutant(which, tag, edits)
    check(f"{tag} {label} is RED", rc not in (0, 98, 99), f"rc {rc}: {out[-160:]}")


def main() -> int:
    if not SAVES:
        print("SKIP (no saves in the corpus or tests/fixtures)")
        return 0
    rc, out = mutant("funnel", "C0", [])
    check("positive control: the unmutated funnel test passes", rc == 0, out[-200:])
    rc, out = mutant("gb", "C1", [])
    check("positive control: the unmutated GB journal test passes", rc == 0, out[-200:])
    J = "jrn_app.c"
    must_fail("funnel", "A1", "pair predicate never fires", [(J, "  if (!ja_older_pairs(&rn)) return 0;", "  if (1) return 0;")])
    must_fail("funnel", "A2", "replaced-slot rule relaxed", [(J, "if (nrep != 1u) return 0;", "if (nrep > 1u) return 0;")])
    must_fail("funnel", "A3", "crossed ignored", [(J, "!r->aux && !r->crossed;", "!r->aux;")])
    must_fail("funnel", "A4", "mismatch tolerated", [(J, "if (s_rec[sp.before + i] != s_sig.after[q]) return 0;", "if (s_rec[sp.before + i] != s_sig.after[q]) continue;")])
    must_fail("funnel", "A5", "minimum compared bytes dropped", [(J, "return match >= JA_MATCH_MIN;", "return match >= 1u;")])
    must_fail("funnel", "A6", "no rollback", [(J, "rb = jrnapp_step(-dir, 0);", "rb = JRN_OK;")])
    must_fail("funnel", "A7", "wrong section stride", [(J, "(uint32_t)(sp->region - JA_PC_FIRST) * G3_SECTOR_DATA_SIZE", "(uint32_t)(sp->region - JA_PC_FIRST) * 3967u")])
    must_fail("funnel", "A8", "History never labels", [(J, "ja_label(rows[n - 1].name, \" 2/2\");", ";"), (J, "ja_label(rows[n].name, \" 1/2\");", ";")])
    must_fail("funnel", "A9", "redo never pairs", [(J, "  if (!p1) return 0;\n  if (p3) {", "  return 0;\n  if (p3) {")])
    # za #314b: chained swaps (Swap + Swap + Box move), tests/host_jrn_funnel_test.c t_swap_chain_*
    must_fail("funnel", "C1", "the chain never extends past two steps", [(J, "  if (max < 3u || !rn.parent || rn.parent + 1u != rm.parent) return 2;", "  if (1) return 2;")])
    must_fail("funnel", "C2", "the chain link's bytes are not checked", [(J, "  if (!ja_older_pairs(&rp)) return 2;", "  if (0) return 2;")])
    must_fail("funnel", "C3", "a chained Swap must add into an EMPTY slot", [(J, "!ja_sig_make(&rn, 0)", "!ja_sig_make(&rn, 1)")])
    must_fail("funnel", "C4", "redo never takes a three-step group", [(J, "    g = ja_group_at(p3, JA_GROUP_MAX);\n    if (g < 0 || g == 3) return g;", "    g = 0;\n    if (g < 0 || g == 3) return g;")])
    must_fail("funnel", "C5", "redo never takes the mid-chain pair", [(J, "    g = ja_group_at(p2, 2u);", "    g = 0;")])
    must_fail("funnel", "C6", "the rollback undoes only one of the applied steps", [(J, "for (k = 0; k < d && rb == JRN_OK; k++)", "for (k = 0; k < 1u && rb == JRN_OK; k++)")])
    must_fail("funnel", "C7", "History never relabels a triple", [(J, "        ja_relabel(rows[n - 2].name, \" 3/3\");\n", "")])
    must_fail("funnel", "C8", "the cap lowered to 2 (no chained group forms)", [(J, "#define JA_GROUP_MAX 3u", "#define JA_GROUP_MAX 2u")])
    # za review A4: the 3-step "half a swap" contract (t_swap_chain_rollback_sweep) -- each survived the suite before that pin
    must_fail("funnel", "H1", "the rollback keeps going after a failed step (a later success hides the half state)", [(J, "for (k = 0; k < d && rb == JRN_OK; k++)", "for (k = 0; k < d; k++)")])
    must_fail("funnel", "H2", "the 'half a swap' contract only for a one-step rollback", [(J, "    if (rb != JRN_OK) {\n      ja_event(\"swap pair: rollback failed", "    if (rb != JRN_OK && d < 2u) {\n      ja_event(\"swap pair: rollback failed")])
    must_fail("funnel", "H3", "a two-step rollback failure returns the error, not JRN_OK", [(J, "      return JRN_OK;                                           /* the image DID change", "      return d < 2u ? JRN_OK : rb;                             /* the image DID change")])
    must_fail("funnel", "M5a", "redo hop cap raised to 4096", [(J, "#define JA_REDO_HOPS  64u", "#define JA_REDO_HOPS  4096u")])
    must_fail("funnel", "M5b", "redo hop cap off by one (hops > cap)", [(J, "hops >= JA_REDO_HOPS) return 0;", "hops > JA_REDO_HOPS) return 0;")])
    must_fail("funnel", "M5c", "redo hop cap off by one the other way (cap - 1)", [(J, "hops >= JA_REDO_HOPS) return 0;", "hops >= JA_REDO_HOPS - 1u) return 0;")])
    must_fail("funnel", "M7a", "pair slot cap raised to 8", [(J, "#define JA_PAIR_SLOTS  4u", "#define JA_PAIR_SLOTS  8u")])
    must_fail("funnel", "M7b", "pair slot cap lowered to 3", [(J, "#define JA_PAIR_SLOTS  4u", "#define JA_PAIR_SLOTS  3u")])
    must_fail("funnel", "R1", "the older half no longer needs the Swap name (the R1 mislabel returns)", [(J, "strcmp(r->name, \"Swap\") == 0; }", "strcmp(r->name, \"Box move\") == 0 || strcmp(r->name, \"Swap\") == 0; }")])
    # zc #321: seq adjacency of a pair's halves
    A1 = "  if (!rm.parent || rm.parent + 1u != newest) return 0;"
    A2 = "  if (max < 3u || !rn.parent || rn.parent + 1u != rm.parent) return 2;"
    A3 = "rows[n - 1].seq == t + 1u && "
    must_fail("funnel", "Q1", "group first link: adjacency dropped", [(J, A1, "  if (!rm.parent) return 0;")])
    must_fail("funnel", "Q2", "group second link: adjacency dropped", [(J, A2, "  if (max < 3u || !rn.parent) return 2;")])
    must_fail("funnel", "Q3", "History link: adjacency dropped", [(J, A3, "")])
    must_fail("funnel", "Q4a", "group gap test lets a one-step gap pair (> instead of !=)", [(J, A1, A1.replace("!= newest", "> newest"))])
    must_fail("funnel", "Q4b", "group gap test demands a gap of 2 (a real pair stops grouping)", [(J, A1, A1.replace("+ 1u", "+ 2u"))])
    must_fail("funnel", "Q4c", "second-link gap test demands a gap of 2", [(J, A2, A2.replace("+ 1u", "+ 2u"))])
    must_fail("funnel", "Q4d", "History gap test demands a gap of 2", [(J, A3, "rows[n - 1].seq == t + 2u && ")])
    # zc #322: a walk read fault refuses the press (t_swap_chain_rollback_sweep deg counters + t_swap_pair_fault_sweep)
    L1 = "return rc == 0 ? 0 : (rc == JRN_E_IO ? JRN_E_IO : 1);"
    must_fail("funnel", "R1w", "locate swallows a read fault (a walk fault degrades to 'no group')", [(J, L1, "return rc == 0 ? 0 : 1;")])
    must_fail("funnel", "R2w", "the record load swallows a read fault", [(J, "  rc = jrn_i_src_read(&s_j, src, 0, s_rec, r->len);\n  " + L1, "  rc = jrn_i_src_read(&s_j, src, 0, s_rec, r->len);\n  return rc == 0 ? 0 : 1;")])
    must_fail("funnel", "R3w", "the redo walk's jrn_find fault is read as 'no group'", [(J, "if (rc != 0) return rc == JRN_E_IO ? rc : 0;", "if (rc != 0) return 0;")])
    must_fail("funnel", "R4w", "jrnapp_step_pair ignores a negative group (falls to a plain step)", [(J, "  if (g < 0) { ja_event(", "  if (g < 0 && 0) { ja_event(")])
    must_fail("funnel", "R5w", "a fault at the THIRD link falls back to the pair", [(J, "  if (rc) return rc < 0 ? rc : 2;\n  if (!ja_older_eligible(&rp)) return 2;", "  if (rc) return 2;\n  if (!ja_older_eligible(&rp)) return 2;")])
    must_fail("funnel", "R6w", "the redo's three-step probe fault falls through to the pair probe", [(J, "if (g < 0 || g == 3) return g;", "if (g == 3) return g;")])
    must_fail("funnel", "R7w", "the redo's pair probe fault is read as 'no group'", [(J, "    if (g < 0) return g;\n    if (g == 2) return 2;", "    if (g == 2) return 2;")])
    # zc #323: the half toast (t_swap_half_toast + the hop-cap leg)
    must_fail("funnel", "T1w", "the (half) mark dropped", [(J, 'memcpy(name, "Swap (half)", 12)', "(void)0")])
    must_fail("funnel", "T2w", "the mark also applied to a plain UNDO", [(J, "rc == JRN_OK && dir > 0 && name", "rc == JRN_OK && name")])
    must_fail("funnel", "T3w", "the whole pair press loses the plain name", [(J, 'memcpy(name, "Swap", 5)', 'memcpy(name, "Swap (half)", 12)')])
    # zc #324(a): the max > 48 clamp of jrnapp_history_tree (t_tree_max_clamp)
    must_fail("funnel", "K1", "the max>48 clamp dropped", [(J, "  if (max > JA_TREE_ROWS) max = JA_TREE_ROWS;\n", "")])
    must_fail("funnel", "K2", "the clamp is one row short", [(J, "if (max > JA_TREE_ROWS) max = JA_TREE_ROWS;", "if (max > JA_TREE_ROWS) max = JA_TREE_ROWS - 1;")])
    G = "gb_jkey.c"
    must_fail("gb", "A10", "GB guard dropped", [(J, "if (s_ai.slot < 0) return 0;                                /* Game Boy: no 80-byte slots, swaps are refused outright */", ""), (J, "return s_ai.slot >= 0 && r->kind", "return r->kind")])
    must_fail("gb", "B1", "no 0xFF escape", [(G, "if (legacy || !has_ff) {", "if (1) {")])
    must_fail("gb", "B2", "compat never probes the old key", [(J, "legacy == key || s_j.ring != 0) return st;", "legacy == key || s_j.ring != 0 || 1) return st;")])
    must_fail("gb", "B3", "adopt without the anchor check", [(J, " && (s_j.anchor == JRN_ANCHOR_MATCH || s_j.anchor == JRN_ANCHOR_BRANCH)) {", ") {")])
    must_fail("gb", "B4", "escaped-name flag dropped", [(G, "(uint16_t)(GBJ_SID_MARK + (unsigned)g), has_ff, 0);", "(uint16_t)(GBJ_SID_MARK + (unsigned)g), 0, 0);")])
    must_fail("gb", "B5", "redirect never written", [(J, "  if (nk == s_key) return;\n  rc = jrn_redirect_write", "  if (1) return;\n  rc = jrn_redirect_write")])
    print(f"{len(fails)} failed" if fails else "all ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
