#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""journal_mutants.py -- mutation proof for the #234 journal host tests (slice 1).

A test that has never failed is decoration. For every assertion class the journal cares about
this tool copies source/journal*.[ch] to a scratch directory, applies ONE mutation to the real
source (the text to change must occur exactly once), rebuilds the named test against the
mutated copy and demands that it goes RED with the named line. A mutant that survives, or that
turns the test red for a different reason, is a hole in the tests and the tool exits non-zero.

    python3 tools/journal_mutants.py            # every mutant
    python3 tools/journal_mutants.py -k crc     # only mutants whose name contains "crc"

The real tree is never touched; the scratch copy lives under a temp directory.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = ["journal.c", "journal.h", "journal_int.h", "journal_undo.c", "journal_fs.c", "journal_fs.h"]


@dataclass(frozen=True)
class Mutant:
    """One mutation: `old` -> `new` in `path`, expected to turn `test` red with `red` in its output."""
    name: str
    path: str
    old: str
    new: str
    test: str      # "journal" (host_journal_test) or "cut" (host_journal_cut_test)
    red: str       # a substring of the RED output that must appear


MUTANTS = [
    Mutant("crc: a flipped CRC is accepted", "journal.c",
           "return jrn_rd32(tail) == crc ? 0 : 2;", "return jrn_rd32(tail) == crc ? 0 : 0;",
           "journal", "a flipped CRC is not accepted"),
    Mutant("torn record: scan trusts a parseable header without its CRC", "journal.c",
           "    rc = rec_at(j, seg, off, &r);\n    if (rc < 0) return rc;\n    if (rc == 0 && (!sc->have",
           "    rc = rec_hdr_at(j, seg, off, &r);\n    if (rc < 0) return rc;\n    if (rc == 0 && (!sc->have",
           "journal", "a flipped CRC is not accepted"),
    Mutant("floor: undo pops a crossed step", "journal_undo.c",
           "  if (r.crossed) return JRN_E_CROSSED;                      /* an undo FLOOR (D6) */\n", "",
           "journal", "undo REFUSES a crossed step"),
    Mutant("floor: redo/re-apply runs through a crossed step", "journal_undo.c",
           "  if (r.crossed) return JRN_E_CROSSED;                      /* redo STOPS before a crossed record */\n", "",
           "journal", "re-apply STOPS before the crossed record"),
    Mutant("floor: orphan-parent floor ignored", "journal_undo.c",
           "  if (r.parent && jrn_i_locate(j, r.parent, &pr, &ps) != 0) return JRN_E_FLOOR;   /* orphan floor */\n",
           "  (void)pr; (void)ps;\n",
           "journal", "undid"),
    Mutant("redirect: a loop is followed to the end", "journal.c",
           "    cur = next;\n  }\n  return JRN_E_LOOP;", "    cur = next;\n  }\n  *out = cur; return JRN_OK;",
           "journal", "a redirect LOOP is detected"),
    Mutant("redirect: an existing redirect is replaced", "journal.c",
           "return have == t ? JRN_OK : JRN_E_EXISTS;", "return JRN_OK;",
           "journal", "never replaced"),
    Mutant("anchor: newest matching hash instead of the last cursor path", "journal.c",
           "for (hops = 0; cur && hops < JRN_WALK_MAX && !found; hops++) {",
           "for (hops = 0; 0 && cur && hops < JRN_WALK_MAX && !found; hops++) {",
           "journal", "anchor follows the LAST CURSOR PATH"),
    Mutant("anchor: redo tip not carried across a session", "journal.c",
           "sc->tip_aux = r->kind == JRN_KIND_CURSOR ? r->aux : r->parent;", "sc->tip_aux = r->parent;",
           "journal", "redo tip 3 across a session"),
    Mutant("marker: undo records no cursor marker", "journal_undo.c",
           "    rc = jrn_i_marker(j, JRN_KIND_CURSOR, j->cursor, j->tip, pre, jrn_hash(j), \"undo\");\n    if (rc) return rc;\n",
           "    (void)pre;\n",
           "journal", "NOT a re-apply offer"),
    Mutant("marker: a discarded marker is not honoured", "journal.c",
           "  } else {\n    sc->c_last = r->parent;", "  } else if (r->kind == JRN_KIND_DISCARD) {\n    sc->expect = r->seq + 1u; sc->have = 1; return;\n  } else {\n    sc->c_last = r->parent;",
           "journal", "a discarded step is NOT re-offered"),
    Mutant("pending: the overflow check is gone (silent overrun)", "journal.c",
           "if ((uint32_t)j->bld_len + need + 4u > (uint32_t)JRN_PEND_CAP - j->pend_len) {",
           "if ((uint32_t)j->bld_len + need + 4u > 1000000u) {",
           "journal", "overran"),
    Mutant("pending: a popped step keeps its seq", "journal.c",
           "  j->pend_n--;\n  j->next_seq--;", "  j->pend_n--;",
           "journal", "seq reclaimed"),
    Mutant("undo: pass 1 skipped (half-applied undo possible)", "journal_undo.c",
           "  rc = span_walk(j, img, s, r, forward, 0, &touched);       /* pass 1: touches nothing */\n  if (rc) return rc;\n",
           "",
           "journal", "no half-applied undo"),
    Mutant("verify: the re-read after a flush is skipped", "journal.c",
           "    rc = seg_verify(j, p.seg[i], p.off[i], j->pend + p.start[i], p.len[i]);",
           "    rc = 0;",
           "journal", "re-read catches a card that dropped the write"),
    Mutant("key: the FRLG layout bit is ignored", "journal.c",
           "  h = fnv_byte(h, frlg);\n", "",
           "journal", "FRLG layout bit"),
    Mutant("segments: prepare creates a segment every call", "journal.c",
           "if (j->seg_last != j->tail_seg || j->seg_last >= SEG_MAX) return JRN_OK;",
           "if (j->seg_last >= SEG_MAX) return JRN_OK;",
           "journal", "a second prepare creates nothing"),
    Mutant("read-only: an Everdrive open zeroes the tail", "journal.c",
           "  if (j->readonly || !j->tail_seg) return 0;\n  rc = zero_from", "  if (!j->tail_seg) return 0;\n  rc = zero_from",
           "journal", "does not even zero the tail"),
    Mutant("verify: a failed re-read does not stop recording", "journal.c",
           "    if (rc == JRN_E_VERIFY) j->stopped = 1;\n", "",
           "journal", "recording STOPS"),
    Mutant("zeroing: the torn tail is left in place", "journal.c",
           "  if (j->readonly || !j->tail_seg) return 0;\n  rc = zero_from(j, j->tail_seg, j->tail_off);",
           "  if (j->readonly || !j->tail_seg) return 0;\n  return 0;\n  rc = zero_from(j, j->tail_seg, j->tail_off);",
           "journal", "torn tail was ZEROED"),
    Mutant("timestamp: the hold is never taken", "journal.c",
           "  s_hold_on = 1;\n  rc = j->fs->write", "  s_hold_on = 0;\n  rc = j->fs->write",
           "cut", "HELD stamp"),
    Mutant("timestamp: the hold is never released", "journal.c",
           "  s_hold_on = 0;\n  return rc == 0 ? 0 : JRN_E_IO;", "  return rc == 0 ? 0 : JRN_E_IO;",
           "cut", "hold is released"),
    Mutant("flush: records written FRONT to back (batch not atomic)", "journal.c",
           "  for (i = (int)j->pend_n - 1; i >= 0; i--) {     /* the FIRST record is the commit point */",
           "  for (i = 0; i < (int)j->pend_n; i++) {          /* the FIRST record is the commit point */",
           "cut", "neither before nor after"),
    Mutant("scan: a later segment's seq is not checked (orphan batch seen)", "journal.c",
           "return rec_at(j, seg, JRN_SEG_HDR, &r) == 0 && (!sc->have || r.seq == sc->expect);",
           "return rec_at(j, seg, JRN_SEG_HDR, &r) == 0;",
           "cut", "neither before nor after"),
    Mutant("scan: a record out of sequence is accepted", "journal.c",
           "if (rc == 0 && (!sc->have || r.seq == sc->expect)) { scan_absorb",
           "if (rc == 0) { scan_absorb",
           "journal", "a stale valid record"),
    Mutant("ring: retire skips the header zero (the oldest stays live)", "journal.c",
           "  memset(z, 0, sizeof z);\n  rc = seg_write(j, idx, 0, z, JRN_SEG_HDR);\n  if (rc) return rc;\n  rc = seg_read(j, idx, 0, b, JRN_SEG_HDR);\n  if (rc) return rc;\n  return memcmp(b, z, sizeof z) == 0 ? 0 : JRN_E_VERIFY;",
           "  (void)z; (void)b; (void)rc; return 0;",
           "journal", "(ring) the retired slot's header is zero in place"),
    Mutant("ring: recycle skips the zero-fill (stale records of the old life)", "journal.c",
           "  rc = slot_zero(j, idx);\n  if (rc) return rc;\n  seg_hdr_build(idx, j->ring, h);",
           "  seg_hdr_build(idx, j->ring, h);",
           "journal", "all-zero body"),
    Mutant("ring: open trusts slot order instead of the header index", "journal.c",
           "&& (!lo || sidx[s - 1u] < lo)) lo = sidx[s - 1u];",
           "&& (!lo)) lo = sidx[s - 1u];",
           "journal", "reopen at step"),
    Mutant("ring: a half-zeroed header (crc gone) is read as live", "journal.c",
           "  if (jrn_crc32_update(0, h, 28) != jrn_rd32(h + 28)) return 0;\n", "",
           "journal", "reads as FREE"),
    Mutant("ring: prepare on a full ring does not retire the oldest", "journal.c",
           "    rc = jrn_compact(j);\n    if (rc) return rc;\n    if ((uint32_t)(j->seg_last",
           "    if ((uint32_t)(j->seg_last",
           "journal", "prepare"),
    Mutant("ring: the header is written BEFORE the slot is zero-filled (cut shows stale body under a live header)", "journal.c",
           "  rc = slot_zero(j, idx);\n  if (rc) return rc;\n  seg_hdr_build(idx, j->ring, h);\n  rc = seg_write(j, idx, 0, h, JRN_SEG_HDR);\n  if (rc) return rc;\n",
           "  seg_hdr_build(idx, j->ring, h);\n  rc = seg_write(j, idx, 0, h, JRN_SEG_HDR);\n  if (rc) return rc;\n  rc = slot_zero(j, idx);\n  if (rc) return rc;\n",
           "cut", "neither before nor after"),
    Mutant("version: a foreign (valid magic+crc, unknown ver/idx/ring) header reads as a FREE slot", "journal.c",
           "*ring > RING_MAX) return SLOT_FOREIGN;", "*ring > RING_MAX) return 0;",
           "journal", "open must say JRN_E_VERSION"),
    Mutant("version: activation zeroes a slot a newer build owns", "journal.c",
           "  if (rc == SLOT_FOREIGN) { j->foreign = 1; return JRN_E_VERSION; }   /* a newer build owns it: never zero it */\n",
           "  if (rc == SLOT_FOREIGN) rc = 0;\n",
           "journal", "activating over a foreign slot refuses"),
    Mutant("format: records start at 32 again (the header sector is shared with the first record)", "journal.c",
           "  j->tail_off = JRN_REC_BASE;", "  j->tail_off = JRN_SEG_HDR;",
           "journal", "a fresh tail starts at the record base"),
    Mutant("first fill: every slot body is zero-filled again (not staged)", "journal.c",
           "f->alloc(f->ctx, p, JRN_SEG_SIZE, 512u) != 0", "f->create_zero(f->ctx, p, JRN_SEG_SIZE, 0, 0) != 0",
           "journal", "the first fill (17 slots) wrote"),
    Mutant("first fill: the body zeroing is dropped from the activation (garbage under a live header)", "journal.c",
           "  rc = slot_zero(j, idx);\n  if (rc) return rc;\n  seg_hdr_build(idx, j->ring, h);",
           "  seg_hdr_build(idx, j->ring, h);",
           "journal", "the activated segment's body is zero"),
    Mutant("bounds: the parent-chain walk is capped at 512 hops again (a long session loses its re-apply offer)", "journal.h",
           "#define JRN_WALK_MAX     (JRN_RING_MAX * ((JRN_SEG_SIZE - JRN_REC_BASE) / JRN_REC_MIN))",
           "#define JRN_WALK_MAX     512u",
           "journal", "unsaved session is offered whole"),
    Mutant("index: locate ignores the first-seq index (always scans from the oldest segment)", "journal.c",
           "  for (s = j->seg_first; s && s <= j->tail_seg; s++) {\n    v = idx_get(j, s);\n    if (v && v <= seq) best = s;\n  }\n  return best;",
           "  (void)v; (void)seq; best = j->seg_first;\n  return best;",
           "journal", "undo"),
    Mutant("index: a flush does not feed the index (first record of a fresh segment)", "journal.c",
           "    if (p.off[i] == JRN_REC_BASE) idx_set(j, p.seg[i], p.seq[i]);",
           "    if (p.off[i] == JRN_REC_BASE) { (void)p.seq[i]; }",
           "journal", "first-seq index a reopen builds"),
    Mutant("index: a retire leaves the retired slot's index behind", "journal.c",
           "    idx_set(j, j->seg_first, 0);\n    j->seg_first++;", "    j->seg_first++;",
           "journal", "first-seq index a reopen builds"),
]


def build(test: str, src_dir: Path, out: Path) -> subprocess.CompletedProcess[str]:
    """Compile host_journal_{test}_test.c against `src_dir`'s journal sources."""
    name = "host_journal_test.c" if test == "journal" else "host_journal_cut_test.c"
    cmd = ["cc", "-std=c11", "-w", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf", "-Dsniprintf=snprintf",
           "-Dvsniprintf=vsnprintf", "-I", str(ROOT / "tests/hostfat"), "-I", str(ROOT / "lib/fatfs"),
           "-I", str(src_dir), str(ROOT / "tests" / name), str(src_dir / "journal.c"),
           str(src_dir / "journal_undo.c"), str(src_dir / "journal_fs.c"), str(ROOT / "lib/fatfs/ff.c"),
           str(ROOT / "lib/fatfs/ffunicode.c"), str(ROOT / "tests/hostfat/ramdisk.c"), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True, check=False)


def run_one(m: Mutant, scratch: Path) -> tuple[bool, str]:
    """Apply `m` to a fresh copy of the sources; True when the test went RED with the wanted line."""
    src = scratch / "src"
    if src.exists():
        shutil.rmtree(src)
    src.mkdir(parents=True)
    for f in FILES:
        shutil.copy(ROOT / "source" / f, src / f)
    text = (src / m.path).read_text()
    if text.count(m.old) != 1:
        return False, f"mutation text occurs {text.count(m.old)} times (must be exactly 1)"
    (src / m.path).write_text(text.replace(m.old, m.new))
    binary = scratch / "mutant_bin"
    b = build(m.test, src, binary)
    if b.returncode != 0:
        return False, "mutant does not build: " + b.stderr.strip().splitlines()[0]
    env = {"JRN_QUICK": "1", "JRN_NO_GARBAGE": "1"}
    r = subprocess.run([str(binary)], capture_output=True, text=True, env=env, check=False, timeout=600)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        return False, "SURVIVED (test stayed green)"
    if m.red not in out:
        first = next((ln for ln in out.splitlines() if ln.startswith("FAIL")), "(no FAIL line)")
        return False, f"red for another reason: {first[:110]}"
    line = next(ln for ln in out.splitlines() if m.red in ln)
    return True, line[:130]


def main() -> int:
    key = sys.argv[sys.argv.index("-k") + 1] if "-k" in sys.argv else ""
    chosen = [m for m in MUTANTS if key in m.name]
    bad = 0
    with tempfile.TemporaryDirectory(prefix="jrn_mut-") as tmp:
        for m in chosen:
            ok, detail = run_one(m, Path(tmp))
            print(f"{'RED ' if ok else 'HOLE'}  {m.name}\n        {detail}")
            bad += 0 if ok else 1
    print(f"\n{len(chosen) - bad}/{len(chosen)} mutants killed" + ("" if not bad else f", {bad} HOLES"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
