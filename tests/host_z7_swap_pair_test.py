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
  za #314/#315 mutants (host_jrn_funnel_test.c): C1..C8 chained swaps, R1 the R1 mislabel (older half no longer needs the "Swap" name)
    A10 the Game Boy guard dropped (a swap-shaped GB pair would pair)                      -> RED
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
    must_fail("funnel", "A1", "pair predicate never fires", [(J, "if (ja_rec_load(&sn, &rn) != 0 || !ja_older_pairs(&rn)) return 0;", "if (1) return 0;")])
    must_fail("funnel", "A2", "replaced-slot rule relaxed", [(J, "if (nrep != 1u) return 0;", "if (nrep > 1u) return 0;")])
    must_fail("funnel", "A3", "crossed ignored", [(J, "!r->aux && !r->crossed;", "!r->aux;")])
    must_fail("funnel", "A4", "mismatch tolerated", [(J, "if (s_rec[sp.before + i] != s_sig.after[q]) return 0;", "if (s_rec[sp.before + i] != s_sig.after[q]) continue;")])
    must_fail("funnel", "A5", "minimum compared bytes dropped", [(J, "return match >= JA_MATCH_MIN;", "return match >= 1u;")])
    must_fail("funnel", "A6", "no rollback", [(J, "rb = jrnapp_step(-dir, 0);", "rb = JRN_OK;")])
    must_fail("funnel", "A7", "wrong section stride", [(J, "(uint32_t)(sp->region - JA_PC_FIRST) * G3_SECTOR_DATA_SIZE", "(uint32_t)(sp->region - JA_PC_FIRST) * 3967u")])
    must_fail("funnel", "A8", "History never labels", [(J, "ja_label(rows[n - 1].name, \" 2/2\");", ";"), (J, "ja_label(rows[n].name, \" 1/2\");", ";")])
    must_fail("funnel", "A9", "redo never pairs", [(J, "  if (!p1) return 0;\n  if (p3) {", "  return 0;\n  if (p3) {")])
    # za #314b: chained swaps (Swap + Swap + Box move), tests/host_jrn_funnel_test.c t_swap_chain_*
    must_fail("funnel", "C1", "the chain never extends past two steps", [(J, "  if (max < 3u || !rn.parent) return 2u;", "  if (1) return 2u;")])
    must_fail("funnel", "C2", "the chain link's bytes are not checked", [(J, "if (ja_rec_load(&sp, &rp) != 0 || !ja_older_pairs(&rp)) return 2u;", "if (ja_rec_load(&sp, &rp) != 0) return 2u;")])
    must_fail("funnel", "C3", "a chained Swap must add into an EMPTY slot", [(J, "!ja_sig_make(&rn, 0)", "!ja_sig_make(&rn, 1)")])
    must_fail("funnel", "C4", "redo never takes a three-step group", [(J, "    g = ja_group_at(p3, JA_GROUP_MAX);\n    if (g == 3u) return 3u;", "    g = 0;\n    if (g == 3u) return 3u;")])
    must_fail("funnel", "C5", "redo never takes the mid-chain pair", [(J, "if (p2 && ja_group_at(p2, 2u) == 2u) return 2u;", "if (0) return 2u;")])
    must_fail("funnel", "C6", "the rollback undoes only one of the applied steps", [(J, "for (k = 0; k < d && rb == JRN_OK; k++)", "for (k = 0; k < 1u && rb == JRN_OK; k++)")])
    must_fail("funnel", "C7", "History never relabels a triple", [(J, "        ja_relabel(rows[n - 2].name, \" 3/3\");\n", "")])
    must_fail("funnel", "C8", "the cap lowered to 2 (no chained group forms)", [(J, "#define JA_GROUP_MAX 3u", "#define JA_GROUP_MAX 2u")])
    must_fail("funnel", "R1", "the older half no longer needs the Swap name (the R1 mislabel returns)", [(J, "strcmp(r->name, \"Swap\") == 0; }", "strcmp(r->name, \"Box move\") == 0 || strcmp(r->name, \"Swap\") == 0; }")])
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
