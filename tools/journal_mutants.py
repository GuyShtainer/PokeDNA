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
    only: str = ""  # when set: run ALL three formats (exFAT hazards need it) but only the scenarios whose name contains this


MUTANTS = [
    Mutant("crc: a flipped CRC is accepted", "journal.c",
           "return jrn_rd32(b + r->len - 4u) == jrn_crc32_update(0, b, (uint32_t)r->len - 4u) ? 0 : 2;", "return jrn_rd32(b + r->len - 4u) == jrn_crc32_update(0, b, (uint32_t)r->len - 4u) ? 0 : 0;",
           "journal", "a flipped CRC is not accepted"),
    Mutant("torn record: scan trusts a parseable header without its CRC", "journal.c",
           "    rc = rec_check(b + p, n - p, &r);\n    if (rc == REC_SHORT && !last) break;                                                /* crosses into the next chunk */\n    if (rc != 0) return 0;",
           "    rc = rec_hdr(b + p, n - p, &r);\n    if (rc == REC_SHORT && !last) break;                                                /* crosses into the next chunk */\n    if (rc != 0) return 0;",
           "journal", "a flipped CRC is not accepted"),
    Mutant("floor: undo pops a crossed step", "journal_undo.c",
           "  if (r.crossed) return JRN_E_CROSSED;                      /* an undo FLOOR (D6) */\n", "",
           "journal", "undo REFUSES a crossed step"),
    Mutant("floor: redo/re-apply runs through a crossed step", "journal_undo.c",
           "  if (r.crossed) return JRN_E_CROSSED;                      /* redo STOPS before a crossed record */\n", "",
           "journal", "re-apply STOPS before the crossed record"),
    Mutant("floor: orphan-parent floor ignored", "journal_undo.c",
           "    if (rc) return JRN_E_FLOOR;                             /* orphan floor */\n",
           "",
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
           "  rc = span_walk(j, img, &ram, r, forward, 0, &touched);    /* pass 1: touches nothing */\n  if (rc) return rc;\n",
           "",
           "journal", "no half-applied undo"),
    Mutant("verify: the re-read after a flush is skipped", "journal.c",
           "    rc = seg_verify(j, p.seg[i], p.off[i], j->pend + p.start[i], p.len[i]);",
           "    rc = 0;",
           "journal", "re-read catches a card that dropped the write"),
    Mutant("key: the FRLG layout bit is ignored", "journal.c",
           "  h = fnv_byte(h, frlg ? 1u : 0u);\n", "",
           "journal", "FRLG layout bit"),
    Mutant("segments: prepare creates a segment every call", "journal.c",
           "  if (j->seg_last != j->tail_seg) return JRN_OK; ",
           "  if (0) return JRN_OK; ",
           "journal", "a second prepare creates nothing"),
    Mutant("read-only: an Everdrive open zeroes the tail", "journal.c",
           "  if (j->readonly || !j->tail_seg) return 0;\n  rc = zero_from", "  if (!j->tail_seg) return 0;\n  rc = zero_from",
           "journal", "does not even zero the tail"),
    Mutant("verify: a failed re-read does not stop recording", "journal.c",
           "    if (rc == JRN_E_VERIFY) j->stopped = 1;\n    if (rc) return rc;\n  }\n  for (i = 0; i < (int)j->pend_n; i++)", "    if (rc) return rc;\n  }\n  for (i = 0; i < (int)j->pend_n; i++)",
           "journal", "recording STOPS"),
    Mutant("zeroing: the torn tail is left in place", "journal.c",
           "  if (j->readonly || !j->tail_seg) return 0;\n  rc = zero_from(j, j->tail_seg, j->tail_off);",
           "  if (j->readonly || !j->tail_seg) return 0;\n  return 0;\n  rc = zero_from(j, j->tail_seg, j->tail_off);",
           "journal", "torn tail was ZEROED"),
    Mutant("timestamp: the hold is never taken", "journal.c",
           "  s_hold_on = 1;\n  rc = j->fs->write", "  s_hold_on = 0;\n  rc = j->fs->write",
           "cut", "HELD stamp"),
    Mutant("timestamp: the hold is never released", "journal.c",
           "  rc = j->fs->write(j->fs->ctx, p, off, buf, n);\n  s_hold_on = 0;", "  rc = j->fs->write(j->fs->ctx, p, off, buf, n);",
           "cut", "hold is released"),
    Mutant("flush: records written FRONT to back (batch not atomic)", "journal.c",
           "  for (i = (int)j->pend_n - 1; i >= 0; i--) {     /* the FIRST record is the commit point */",
           "  for (i = 0; i < (int)j->pend_n; i++) {          /* the FIRST record is the commit point */",
           "cut", "neither before nor after"),
    Mutant("scan: a later segment's seq is not checked (orphan batch seen)", "journal.c",
           "c->ok = rec_check(b, n, &r) == 0 && r.kind != JRN_KIND_PART && (!c->sc->have || r.seq == c->sc->expect);",
           "c->ok = rec_check(b, n, &r) == 0 && r.kind != JRN_KIND_PART;",
           "cut", "neither before nor after"),
    Mutant("scan: a record out of sequence is accepted", "journal.c",
           "if (c->sc->have && r.seq != c->sc->expect) return 0;                               /* this segment's run ends */",
           "",
           "journal", "a stale valid record"),
    Mutant("ring: retire skips the header zero (the oldest stays live)", "journal.c",
           "  memset(z, 0, sizeof z);\n  rc = seg_write(j, idx, 0, z, JRN_SEG_HDR);\n  if (rc) return rc;\n  rc = seg_read(j, idx, 0, b, JRN_SEG_HDR);\n  if (rc) return rc;\n  return memcmp(b, z, sizeof z) == 0 ? 0 : JRN_E_VERIFY;",
           "  (void)z; (void)b; (void)rc; return 0;",
           "journal", "(ring) the retired slot's header is zero in place"),
    Mutant("ring: recycle skips the zero-fill (stale records of the old life)", "journal.c",
           "  rc = slot_zero(j, idx);\n  if (rc) return rc;\n  seg_hdr_build(idx, j->ring, ver, j->nreg, j->reg_size, h);",
           "  seg_hdr_build(idx, j->ring, ver, j->nreg, j->reg_size, h);",
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
           "  rc = slot_zero(j, idx);\n  if (rc) return rc;\n  seg_hdr_build(idx, j->ring, ver, j->nreg, j->reg_size, h);\n  rc = seg_write(j, idx, 0, h, JRN_SEG_HDR);\n  if (rc) return rc;\n",
           "  seg_hdr_build(idx, j->ring, ver, j->nreg, j->reg_size, h);\n  rc = seg_write(j, idx, 0, h, JRN_SEG_HDR);\n  if (rc) return rc;\n  rc = slot_zero(j, idx);\n  if (rc) return rc;\n",
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
           "  rc = slot_zero(j, idx);\n  if (rc) return rc;\n  seg_hdr_build(idx, j->ring, ver, j->nreg, j->reg_size, h);",
           "  seg_hdr_build(idx, j->ring, ver, j->nreg, j->reg_size, h);",
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
    Mutant("read error: a failed directory listing reads as an empty journal", "journal.c",
           "  if (rc < 0) return JRN_E_IO;                                   /* a card error is not an empty journal */",
           "  if (rc < 0) { j->anchor = JRN_ANCHOR_EMPTY; return JRN_OK; }",
           "journal", "SILENT wrong views"),
    Mutant("read error: a failed slot-header read is a free slot (a shorter ring)", "journal.c",
           "    if (rc < 0) return rc == SLOT_FOREIGN ? JRN_E_VERSION : rc;",
           "    if (rc == JRN_E_IO) rc = 0;\n    if (rc < 0) return rc == SLOT_FOREIGN ? JRN_E_VERSION : rc;",
           "journal", "SILENT wrong views"),
    Mutant("read error: a failed peek at the next segment ends the journal there", "journal.c",
           "    rc = next_seg_continues(j, (uint16_t)(seg + 1u), sc);\n    if (rc < 0) return rc;",
           "    rc = next_seg_continues(j, (uint16_t)(seg + 1u), sc);\n    if (rc < 0) rc = 0;",
           "journal", "SILENT wrong views"),
    Mutant("read error: a failed redirect read is an absent redirect (the wrong key directory)", "journal.c",
           "  if (fs->read(fs->ctx, p, 0, b, PDR_LEN) != 0) return JRN_E_IO;",
           "  if (fs->read(fs->ctx, p, 0, b, PDR_LEN) != 0) return 1;",
           "journal", "WRONG key"),
    Mutant("flush: a write that fails part way does not stop recording (pop reclaims a seq with orphans on disk)", "journal.c",
           "    if (rc) { j->stopped = 1; return rc; }\n  }\n  for (i = 0; i < (int)j->pend_n; i++) {",
           "    if (rc) return rc;\n  }\n  for (i = 0; i < (int)j->pend_n; i++) {",
           "journal", "fails part way STOPS"),
    Mutant("recording: a step may begin with no tail segment (buffers for a flush that cannot happen)", "journal.c",
           "  if (!j->tail_seg) return JRN_E_NOSEG;             /* no segment to record into: refuse at the door, never buffer for a flush that cannot happen */\n",
           "",
           "journal", "says NOSEG"),
    Mutant("cap: prepare at SEG_MAX returns OK with no spare (silent)", "journal.c",
           "  if (j->seg_last >= SEG_MAX) return JRN_E_FULL;                           /* the logical index space is spent: LOUD, never a silent no-spare */",
           "  if (j->seg_last >= SEG_MAX) return JRN_OK;",
           "journal", "at the cap says JRN_E_FULL"),
    Mutant("pdr: the redirect is written beside the key directories again (not in <root>/r/)", "journal.c",
           '  if (pput(out, &n, root) || pput(out, &n, "/r/") || pput(out, &n, hex)) return -1;\n  return pput(out, &n, ".pdr");',
           '  if (pput(out, &n, root) || pput(out, &n, "/") || pput(out, &n, hex)) return -1;\n  return pput(out, &n, ".pdr");',
           "journal", "16-byte file in <root>/r/"),
    Mutant("pdr: the version byte is not checked (a newer redirect is followed as if it were ours)", "journal.c",
           "  if (b[3] != JRN_PDR_VER) return JRN_E_VERSION;\n", "",
           "journal", "foreign redirect version"),
    Mutant("pdr: a damaged <root>/r directory blocks the journal open (persistent FR_INT_ERR treated as a card error)", "journal.c",
           "  if (sz == -3) return 1;                 /* a damaged <root>/r directory (torn create): no redirect, the journal keeps its own key */\n",
           "",
           "cut", "neither before nor after", only="pdr"),
    Mutant("repair: an orphan record past the tail (a crossing flush's next segment) is left in place and re-linked", "journal.c",
           "    rc = zero_from(j, s, JRN_REC_BASE);   /* the whole window: a chain rolled into a spare lays its parts down BEFORE its head, so the first record's slot can still be clean while parts stand behind it */\n    if (rc) return rc;",
           "    (void)rc;",
           "cut", "an orphan was re-linked"),
    Mutant("layout: a header written for another region layout is read as ours (silent NEWROOT / misread)", "journal.c",
           "  if (rc == 1 && (nreg != j->nreg || rsz != j->reg_size)) return SLOT_FOREIGN;",
           "  if (rc == 1 && (nreg != j->nreg || rsz != j->reg_size)) return rc;",
           "journal", "JRN_E_VERSION, not a new root"),
    Mutant("names: a step name with control / non-ASCII bytes is accepted", "journal.c",
           "    if ((uint8_t)name[i] < 0x20u || (uint8_t)name[i] > 0x7Eu) return JRN_E_ARG;",
           "    if (0) return JRN_E_ARG;",
           "journal", "control / non-ASCII bytes"),
    Mutant("key: the name is not canonicalized (0xFF padding / garbage after the terminator moves the key)", "journal.c",
           "i < name_len && name[i] != 0xFFu; i++)", "i < name_len; i++)",
           "journal", "padded name"),
    Mutant("crc loop #302: a region with a changed byte reuses the old crc as its new one", "journal.c",
           "changed ? jrn_crc32_update(0, new_blk, j->reg_size) : j->crc[region]", "j->crc[region]",
           "journal", "tracked hash == image hash"),
    Mutant("crc loop #302: next_run skips one byte past an identical 64-byte window", "journal.c",
           "memcmp(o + i, n + i, 64u) == 0) i += 64u;", "memcmp(o + i, n + i, 64u) == 0) i += 65u;",
           "journal", "the lone byte is found"),
    Mutant("crc loop #302: next_run compares only half of each 64-byte window", "journal.c",
           "memcmp(o + i, n + i, 64u) == 0) i += 64u;", "memcmp(o + i, n + i, 32u) == 0) i += 64u;",
           "journal", "the lone byte is found"),
    Mutant("crc loop #302: an untouched region skips the old-hash (DIVERGED) check", "journal.c",
           "  if (jrn_crc32_update(0, old_blk, j->reg_size) != j->crc[region]) { jrn_step_abort(j); return JRN_E_DIVERGED; }\n", "",
           "journal", "never reuses crc[region]"),
    Mutant("#316: pass 2 streams the record from the CARD again (torn image on a mid-apply read fault)", "journal_undo.c",
           "rc = span_walk(j, img, &ram, r, forward, 1, &touched);", "rc = span_walk(j, img, s, r, forward, 1, &touched);",
           "journal", "TORN image"),
    Mutant("#316: the crc check reads the card (a read fault surfaces as JRN_E_STATE, not JRN_E_IO)", "journal_undo.c",
           "if (!src_crc_ok(j, &ram, r)) return JRN_E_STATE;", "if (!src_crc_ok(j, s, r)) return JRN_E_STATE;",
           "journal", "want JRN_E_IO"),
    Mutant("#316: the RAM copy is partial (the last 4 bytes never copied)", "journal_undo.c",
           "jrn_i_src_read(j, s, 0, rec, r->len) != 0", "jrn_i_src_read(j, s, 0, rec, r->len - 4u) != 0",
           "journal", "the healthy press"),
    Mutant("#316 review: the crc check on the RAM copy is skipped (a rotted record body is applied)", "journal_undo.c",
           "  if (!src_crc_ok(j, &ram, r)) return JRN_E_STATE;\n", "",
           "journal", "rotted record body was APPLIED"),
    # ---- z9 (#301 v2 / #307): chained multi-record steps ----------------------------------------------------------
    Mutant("chain: a part may name any head", "journal.c",
           "r->seq == nxt && r->parent == h->seq && r->aux == nxt - h->seq && !r->crossed;", "r->seq == nxt && r->aux == nxt - h->seq && !r->crossed;",
           "chain", "FAIL"),
    Mutant("chain: a part's index is not checked (reader)", "journal.c",
           "r->seq == nxt && r->parent == h->seq && r->aux == nxt - h->seq && !r->crossed;", "r->seq == nxt && r->parent == h->seq && !r->crossed;",
           "chain", "FAIL"),
    Mutant("chain: a head with a count past JRN_CHAIN_MAX is held", "journal.c",
           "if (r.aux < 2u || r.aux > JRN_CHAIN_MAX) return 0;", "if (r.aux < 2u) return 0;",
           "chain", "FAIL"),
    Mutant("chain: an unfinished chain is absorbed at its head", "journal.c",
           "      c->head = r; c->head_off = off + p; c->want = (uint8_t)(r.aux - 1u); c->nxt = r.seq + 1u;\n      p += r.len;\n      continue;",
           "      c->head = r; c->head_off = off + p; c->want = (uint8_t)(r.aux - 1u); c->nxt = r.seq + 1u;\n      scan_absorb(c->sc, &r, c->hash); c->off = off + p + r.len;\n      p += r.len;\n      continue;",
           "chain", "FAIL"),
    Mutant("chain: the open zeroes one record past the tail, not a chain's worth", "journal.c",
           "JRN_SEG_SIZE - from < ZWINDOW ? JRN_SEG_SIZE - from : ZWINDOW);", "JRN_SEG_SIZE - from < JRN_REC_MAX ? JRN_SEG_SIZE - from : JRN_REC_MAX);",
           "chain", "FAIL"),
    Mutant("chain: the writer's cap refuses a whole 8-record chain", "journal.c",
           "if (p->n >= JRN_CHAIN_MAX) return JRN_E_TOOBIG;", "if (p->n >= JRN_CHAIN_MAX - 1) return JRN_E_TOOBIG;",
           "chain", "FAIL"),
    Mutant("chain: the head is written first", "journal.c",
           "  for (k = p->n; k-- > 0;) {\n    for (o = off, r = 0; r < k; r++) o += p->len[r];\n    ch_build(j, blk, nblk, p, k, name, crossed, pre, post);\n    rc = seg_write",
           "  for (k = 0; k < p->n; k++) {\n    for (o = off, r = 0; r < k; r++) o += p->len[r];\n    ch_build(j, blk, nblk, p, k, name, crossed, pre, post);\n    rc = seg_write",
           "cut", "FAIL"),
    Mutant("chain: the write-back verify is skipped", "journal.c",
           "  if (!rc) rc = ch_verify_all(j, blk, nblk, &p, name, crossed, pre, post, seg, off);", "",
           "chain", "FAIL"),
    Mutant("chain: apply trusts the chain without phase A", "journal_undo.c",
           "    if (!chain_member(&r, head, k)) return JRN_E_STATE;\n    rc = span_walk(j, img, &ram, &r, forward, 0, &touched);\n    if (rc) return rc;",
           "    if (!chain_member(&r, head, k)) return JRN_E_STATE;",
           "chain", "FAIL"),
    Mutant("chain: a failed apply is not rolled back", "journal_undo.c",
           "    for (back = (int)done - 1; back >= 0 && !torn; back--) {", "    for (back = (int)done - 1; 0 && back >= 0 && !torn; back--) {",
           "chain", "FAIL"),
    Mutant("chain: a failed rollback is not TORN", "journal_undo.c",
           "  if (torn) return JRN_E_TORN;", "",
           "chain", "FAIL"),
    Mutant("chain: a new spare is not stamped v2", "journal.c",
           "  if (n >= 2u && v != (int)JRN_SEG_VER2) {", "  if (0 && n >= 2u && v != (int)JRN_SEG_VER2) {",
           "chain", "FAIL"),
    Mutant("chain: an unknown version (3) is read as live", "journal.c",
           "(jrn_rd16(h + 4) != SEG_VER && jrn_rd16(h + 4) != JRN_SEG_VER2)", "(jrn_rd16(h + 4) != SEG_VER && jrn_rd16(h + 4) != JRN_SEG_VER2 && jrn_rd16(h + 4) != 3)",
           "journal", "FAIL"),
    # ---- #304 the children walk (tests/host_jrn_kids_test.c) ----
    Mutant("kids: the walk counts the branch child as its own sibling (skip ignored)", "journal.c",
           "if (r->seq != c->skip[mid] && c->cnt[mid] < 0xFFFFu) c->cnt[mid]++;", "if (c->cnt[mid] < 0xFFFFu) c->cnt[mid]++;",
           "kids", "FORK-2: A's children other than the branch child C = 1"),
    Mutant("kids: the list ignores skip (self-inclusion)", "journal.c",
           "if (r->parent != c->want || r->seq == c->wskip) return 1;", "if (r->parent != c->want) return 1;",
           "kids", "FORK-2: the listed sibling is B"),
    Mutant("kids: the TAIL segment is not scanned", "journal.c",
           "for (; s <= j->tail_seg && !c->done; s++) {", "for (; s < j->tail_seg && !c->done; s++) {",
           "kids", "TAIL/FLUSHED"),
    Mutant("kids: the pending buffer is not walked", "journal.c",
           "for (i = 0; i < j->pend_n && !c->done; i++) {", "for (i = 0; 0 && i < j->pend_n && !c->done; i++) {",
           "kids", "TAIL/PENDING"),
    Mutant("kids: the walk starts one segment too late", "journal.c",
           "    s = seg_for_seq(j, from);\n    if (!s) s = j->seg_first;\n    for (; s <= j->tail_seg && !c->done; s++) {",
           "    s = (uint16_t)(seg_for_seq(j, from) + 1u);\n    if (!s) s = j->seg_first;\n    for (; s <= j->tail_seg && !c->done; s++) {",
           "kids", "TAIL/FLUSHED"),
    Mutant("kids: a chain PART counts as a child", "journal.c",
           "  if (r->kind != JRN_KIND_STEP) return 1;                                            /* parts and markers are never children */",
           "  if (r->kind != JRN_KIND_STEP && r->kind != JRN_KIND_PART) return 1;",
           "kids", "CHAIN: the head has NO children"),
    Mutant("kids: a cursor MARKER counts as a child", "journal.c",
           "  if (r->kind != JRN_KIND_STEP) return 1;                                            /* parts and markers are never children */",
           "  if (r->kind != JRN_KIND_STEP && r->kind != JRN_KIND_CURSOR) return 1;",
           "kids", "FORK-2"),
    Mutant("kids: counts are not reset (a stale counter survives a call)", "journal.c",
           "  memset(count, 0, (size_t)n * sizeof count[0]);\n", "",
           "kids", "FORK-2: A's children other than the branch child C = 1"),
    Mutant("kids: a non-descending parent list is accepted", "journal.c",
           "if (parent[i] >= parent[i - 1u]) return JRN_E_ARG;", "if (0) return JRN_E_ARG;",
           "kids", "equal parents are refused"),
    Mutant("kids: list paging off by one (first is exclusive)", "journal.c",
           "if (c->seen++ >= c->first) {", "if (c->seen++ > c->first) {",
           "kids", "list page"),
]


def build(test: str, src_dir: Path, out: Path) -> subprocess.CompletedProcess[str]:
    """Compile host_journal_{test}_test.c against `src_dir`'s journal sources."""
    name = {"journal": "host_journal_test.c", "chain": "host_jrn_chain_test.c", "kids": "host_jrn_kids_test.c"}.get(test, "host_journal_cut_test.c")
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
    env = {"JRN_NO_GARBAGE": "1"}
    if m.only:
        env["JRN_ONLY"] = m.only
    else:
        env["JRN_QUICK"] = "1"
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
