/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal_undo.c -- the #234 undo/redo primitives (design D5/D6). PURE C.
 *
 * The cursor rule is never bent: a record applies only if the image bytes at ALL its spans
 * equal its `after` (undo) / `before` (redo). Pass 1 verifies every span and touches
 * nothing; only then does pass 2 write. The engine never owns the image: it reads and
 * writes through the caller's JrnImage. Undo/redo patch the image and record a CURSOR
 * marker; they never go through the caller's staging funnel. */
#include <string.h>

#include "journal.h"
#include "journal_int.h"

#define CH 64u   /* the stack chunk a span is streamed through (two of them) */

/* The record's own CRC must hold before a single byte is applied. */
static int src_crc_ok(const Jrn* j, const JrnSrc* s, const JrnRec* r) {
  uint8_t chunk[CH], tail[4];
  uint32_t crc = 0, c, m, body = (uint32_t)r->len - 4u;
  for (c = 0; c < body; c += m) {
    m = body - c < CH ? body - c : CH;
    if (jrn_i_src_read(j, s, c, chunk, m) != 0) return 0;
    crc = jrn_crc32_update(crc, chunk, m);
  }
  if (jrn_i_src_read(j, s, body, tail, 4) != 0) return 0;
  return jrn_rd32(tail) == crc;
}

/* One pass over a record's spans. forward = redo (image must hold `before`, write `after`);
 * !forward = undo (image must hold `after`, write `before`). apply == 0 is the verify pass:
 * it compares and never writes. *touched collects the regions the record spans. */
static int span_walk(const Jrn* j, const JrnImage* img, const JrnSrc* s, const JrnRec* r,
                     int forward, int apply, uint16_t* touched) {
  uint8_t hdr[JRN_SPAN_HDR], a[CH], b[CH];
  uint32_t pos = JRN_REC_HDR, end = (uint32_t)r->len - 4u, c, m, exp_off, tgt_off;
  uint16_t off, len;
  uint8_t sp, region;
  *touched = 0;
  for (sp = 0; sp < r->nspans; sp++) {
    if (pos + JRN_SPAN_HDR > end) return JRN_E_STATE;
    if (jrn_i_src_read(j, s, pos, hdr, JRN_SPAN_HDR) != 0) return JRN_E_IO;
    region = hdr[0]; off = jrn_rd16(hdr + 2); len = jrn_rd16(hdr + 4);
    if (region >= j->nreg || !len || (uint32_t)off + len > j->reg_size ||
        pos + JRN_SPAN_HDR + 2u * len > end) return JRN_E_STATE;
    exp_off = pos + JRN_SPAN_HDR + (forward ? 0u : len);
    tgt_off = pos + JRN_SPAN_HDR + (forward ? len : 0u);
    for (c = 0; c < len; c += m) {
      m = len - c < CH ? len - c : CH;
      if (!apply) {
        if (img->get(img->ctx, region, (uint16_t)(off + c), a, (uint16_t)m) != 0) return JRN_E_IO;
        if (jrn_i_src_read(j, s, exp_off + c, b, m) != 0) return JRN_E_IO;
        if (memcmp(a, b, m) != 0) return JRN_E_DIVERGED;
      } else {
        if (jrn_i_src_read(j, s, tgt_off + c, b, m) != 0) return JRN_E_IO;
        if (img->set(img->ctx, region, (uint16_t)(off + c), b, (uint16_t)m) != 0) return JRN_E_IO;
      }
    }
    *touched = (uint16_t)(*touched | (1u << region));
    pos += JRN_SPAN_HDR + 2u * len;
  }
  return pos == end ? 0 : JRN_E_STATE;
}

/* Verify, then apply, then re-derive the crc of every touched region from the image.
 * #316: the record is read from the card ONCE into a RAM copy (<= JRN_REC_MAX); the crc check, pass 1 and
 * pass 2 all run from that copy. A card read fault therefore aborts BEFORE a single image byte changes, and
 * pass 2 can no longer be cut short by a read error halfway through a patch. */
static int apply_record(Jrn* j, const JrnImage* img, const JrnSrc* s, const JrnRec* r, int forward) {
  uint8_t rec[JRN_REC_MAX];
  JrnSrc ram;
  uint16_t touched;
  uint8_t g;
  int rc, rc2 = 0;
  if (r->len < JRN_REC_MIN || r->len > JRN_REC_MAX) return JRN_E_STATE;
  if (s->ram) {
    ram = *s;                                               /* already in RAM (the pending buffer) */
  } else {
    if (jrn_i_src_read(j, s, 0, rec, r->len) != 0) return JRN_E_IO;   /* the only card read of the apply */
    memset(&ram, 0, sizeof ram);
    ram.ram = rec;
  }
  if (!src_crc_ok(j, &ram, r)) return JRN_E_STATE;
  rc = span_walk(j, img, &ram, r, forward, 0, &touched);    /* pass 1: touches nothing */
  if (rc) return rc;
  rc = span_walk(j, img, &ram, r, forward, 1, &touched);    /* pass 2: from RAM, no card read can interrupt it */
  for (g = 0; g < j->nreg; g++)
    if (touched & (1u << g)) {
      rc2 = jrn_i_region_crc(j, img, g, &j->crc[g]);
      if (rc2) break;
    }
  return rc ? rc : rc2;
}

/* ---- CHAINED steps (D10): apply a head + its parts as ONE all-or-nothing step -------------------------------------------------
 * The chain lives in one segment: the head at hs->base, each part right behind the previous record. Records are read into the ONE 512-byte
 * stack buffer (#316) one at a time.
 *   PHASE A (verify): every record is read, header-checked against the head (seq / parent / aux / kind), CRC-checked and span-verified against the
 *     image (undo: image == after, redo: image == before). Nothing is written; any failure leaves the image untouched.
 *   PHASE B (apply): the records are read AGAIN (a second read of the card is never trusted to equal the first: the CRC and the header are checked
 *     again) and patched in: undo head -> parts, redo parts -> head (the spans of a chain are disjoint by construction, so the order only matters for
 *     the rollback bookkeeping). A failure part way ROLLS BACK every record already applied (and the failing record itself when a patch started),
 *     newest first, by applying the opposite direction. If the rollback itself fails the image holds a PARTIAL step: JRN_E_TORN, and the caller
 *     re-derives its copies and floors the history (jrn_app.c). The cursor never moves on any failure. */
static int chain_load(const Jrn* j, uint16_t seg, uint32_t pos, uint8_t* rec, JrnRec* r) {
  JrnSrc s;
  JrnSrc ram;
  uint32_t n = JRN_SEG_SIZE - pos < JRN_REC_MAX ? JRN_SEG_SIZE - pos : JRN_REC_MAX;
  if (pos < JRN_REC_BASE || n < JRN_REC_MIN) return JRN_E_STATE;
  memset(&s, 0, sizeof s);
  s.seg = seg; s.base = pos;
  if (jrn_i_src_read(j, &s, 0, rec, n) != 0) return JRN_E_IO;
  if (jrn_i_hdr_parse(rec, r) != 0 || r->len > n) return JRN_E_STATE;
  memset(&ram, 0, sizeof ram);
  ram.ram = rec;
  return src_crc_ok(j, &ram, r) ? 0 : JRN_E_STATE;
}

/* Is `r` (just loaded) really record k of the chain headed by `h`? */
static int chain_member(const JrnRec* r, const JrnRec* h, uint8_t k) {
  if (!k) return r->kind == JRN_KIND_STEP && r->seq == h->seq && r->aux == h->aux && r->parent == h->parent;
  return r->kind == JRN_KIND_PART && r->seq == h->seq + k && r->parent == h->seq && r->aux == k && !r->crossed;
}

static int chain_apply(Jrn* j, const JrnImage* img, const JrnSrc* hs, const JrnRec* head, int forward) {
  uint8_t rec[JRN_REC_MAX];
  uint16_t len[JRN_CHAIN_MAX], touched, all = 0;
  JrnRec r;
  JrnSrc ram;
  uint32_t pos = hs->base, base;
  uint8_t n = (uint8_t)head->aux, k, g, done = 0, i;
  int rc = 0, rc2 = 0, back, step, torn = 0;
  if (hs->ram || head->kind != JRN_KIND_STEP || head->aux < 2u || head->aux > JRN_CHAIN_MAX) return JRN_E_STATE;
  memset(&ram, 0, sizeof ram);
  ram.ram = rec;
  for (k = 0; k < n; k++) {                                                    /* phase A: verify everything, touch nothing */
    rc = chain_load(j, hs->seg, pos, rec, &r);
    if (rc) return rc;
    if (!chain_member(&r, head, k)) return JRN_E_STATE;
    rc = span_walk(j, img, &ram, &r, forward, 0, &touched);
    if (rc) return rc;
    len[k] = r.len; pos += r.len;
  }
  for (i = 0; i < n; i++) {                                                    /* phase B: apply (undo ascending, redo descending) */
    k = forward ? (uint8_t)(n - 1u - i) : i;
    for (base = hs->base, g = 0; g < k; g++) base += len[g];
    rc = chain_load(j, hs->seg, base, rec, &r);
    if (!rc && (!chain_member(&r, head, k) || r.len != len[k])) rc = JRN_E_STATE;
    if (rc) break;
    rc = span_walk(j, img, &ram, &r, forward, 1, &touched);
    all = (uint16_t)(all | touched);
    if (rc) {                                                                  /* a patch began: undo THIS record from the RAM copy first */
      if (span_walk(j, img, &ram, &r, !forward, 1, &touched) != 0) torn = 1;
      break;
    }
    done++;
  }
  if (rc) {                                                                    /* roll back the records already applied, newest first */
    for (back = (int)done - 1; back >= 0 && !torn; back--) {
      step = back;
      k = forward ? (uint8_t)(n - 1u - (uint8_t)step) : (uint8_t)step;
      for (base = hs->base, g = 0; g < k; g++) base += len[g];
      if (chain_load(j, hs->seg, base, rec, &r) != 0 || !chain_member(&r, head, k) || r.len != len[k] ||
          span_walk(j, img, &ram, &r, !forward, 1, &touched) != 0) torn = 1;
    }
  }
  if (rc) all = (uint16_t)((1u << j->nreg) - 1u);                              /* a failed patch may have stopped mid-span: re-derive every region's crc */
  for (g = 0; g < j->nreg; g++)
    if (all & (1u << g)) {
      rc2 = jrn_i_region_crc(j, img, g, &j->crc[g]);
      if (rc2) break;
    }
  if (torn) return JRN_E_TORN;
  return rc ? rc : rc2;
}

static int guard_args(const Jrn* j, const JrnImage* img) {
  if (!j || !img || !img->get || !img->set) return JRN_E_ARG;
  if (j->readonly) return JRN_E_RDONLY;
  if (j->stopped) return JRN_E_STOPPED;
  if (j->bld_len) return JRN_E_STATE;
  return 0;
}

int jrn_undo(Jrn* j, const JrnImage* img, JrnRec* undone) {
  JrnRec r, pr, lastr;
  JrnSrc s, ps;
  uint16_t start = 0;
  uint32_t pre, oldcur, oldtip;
  int rc = guard_args(j, img), pending;
  if (rc) return rc;
  if (!j->cursor) return JRN_E_NOTHING;
  rc = jrn_i_locate(j, j->cursor, &r, &s);
  if (rc) return rc;
  if (r.crossed) return JRN_E_CROSSED;                      /* an undo FLOOR (D6) */
  if (r.parent) {
    rc = jrn_i_locate(j, r.parent, &pr, &ps);
    if (rc == JRN_E_IO) return rc;                          /* #317: a flaky read is an I/O error, not "crossed into another file" */
    if (rc) return JRN_E_FLOOR;                             /* orphan floor */
  }
  pending = s.ram != 0;
  if (pending) {
    rc = jrn_i_pend_last(j, &start, &lastr);
    if (rc || lastr.seq != r.seq) return JRN_E_STATE;
  } else if (!jrn_i_marker_room(j)) {
    j->flush_wanted = 1;                                    /* flush first, then retry */
    return JRN_E_FULL;
  }
  pre = jrn_hash(j);
  rc = r.aux ? chain_apply(j, img, &s, &r, 0) : apply_record(j, img, &s, &r, 0);   /* aux != 0: a chain head (D10) */
  if (rc) return rc;
  oldcur = j->cursor; oldtip = j->tip;
  j->cursor = r.parent;
  j->offer = 0;                                             /* the user chose to be here */
  if (pending) {
    jrn_i_pend_pop(j, start);                               /* never hit the disk: no SD I/O */
    j->tip = j->cursor;
  } else {
    j->tip = (oldtip == oldcur) ? oldcur : oldtip;          /* redo target: where we undid FROM */
    rc = jrn_i_marker(j, JRN_KIND_CURSOR, j->cursor, j->tip, pre, jrn_hash(j), "undo");
    if (rc) return rc;
  }
  if (undone) *undone = r;
  return JRN_OK;
}

/* The record right above the cursor on the way to the tip. */
static int child_toward_tip(Jrn* j, JrnRec* out, JrnSrc* s) {
  uint32_t t = j->tip, hops;
  int rc;
  for (hops = 0; hops < JRN_WALK_MAX; hops++) {
    if (!t) return JRN_E_DIVERGED;                          /* the tip is not below the cursor */
    rc = jrn_i_locate(j, t, out, s);
    if (rc) return rc;
    if (out->parent == j->cursor) return JRN_OK;
    t = out->parent;
  }
  return JRN_E_LOOP;
}

int jrn_redo(Jrn* j, const JrnImage* img, JrnRec* redone) {
  JrnRec r;
  JrnSrc s;
  uint32_t pre;
  int rc = guard_args(j, img);
  if (rc) return rc;
  if (j->cursor == j->tip) return JRN_NOOP;
  rc = child_toward_tip(j, &r, &s);
  if (rc) return rc;
  if (r.crossed) return JRN_E_CROSSED;                      /* redo STOPS before a crossed record */
  if (!jrn_i_marker_room(j)) { j->flush_wanted = 1; return JRN_E_FULL; }
  pre = jrn_hash(j);
  rc = r.aux ? chain_apply(j, img, &s, &r, 1) : apply_record(j, img, &s, &r, 1);
  if (rc) return rc;
  j->cursor = r.seq;
  if (j->cursor == j->tip) j->offer = 0;                    /* the offer is fully applied */
  rc = jrn_i_marker(j, JRN_KIND_CURSOR, j->cursor, j->tip, pre, jrn_hash(j), "redo");
  if (rc) return rc;
  if (redone) *redone = r;
  return JRN_OK;
}

int jrn_redo_info(Jrn* j, uint32_t* avail, uint32_t* total, JrnRec* stop) {
  JrnRec r, low;
  JrnSrc s;
  uint32_t t, n = 0, cross_pos = 0, hops;
  int rc;
  if (!j || !avail || !total) return JRN_E_ARG;
  t = j->tip;
  memset(&low, 0, sizeof low);
  for (hops = 0; t != j->cursor && hops < JRN_WALK_MAX; hops++) {
    if (!t) return JRN_E_DIVERGED;
    rc = jrn_i_locate(j, t, &r, &s);
    if (rc) return rc;
    n++;
    if (r.crossed) { cross_pos = n; low = r; }              /* the LAST seen is the lowest */
    t = r.parent;
  }
  if (t != j->cursor) return JRN_E_LOOP;
  *total = n;
  *avail = cross_pos ? n - cross_pos : n;
  if (stop && cross_pos) *stop = low;
  return cross_pos ? 1 : 0;                                 /* 1 = a crossed record cuts the chain */
}
