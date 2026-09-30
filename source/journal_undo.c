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

/* Verify, then apply, then re-derive the crc of every touched region from the image. */
static int apply_record(Jrn* j, const JrnImage* img, const JrnSrc* s, const JrnRec* r, int forward) {
  uint16_t touched;
  uint8_t g;
  int rc, rc2 = 0;
  if (!src_crc_ok(j, s, r)) return JRN_E_STATE;
  rc = span_walk(j, img, s, r, forward, 0, &touched);       /* pass 1: touches nothing */
  if (rc) return rc;
  rc = span_walk(j, img, s, r, forward, 1, &touched);       /* pass 2 */
  for (g = 0; g < j->nreg; g++)
    if (touched & (1u << g)) {
      rc2 = jrn_i_region_crc(j, img, g, &j->crc[g]);
      if (rc2) break;
    }
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
  if (r.parent && jrn_i_locate(j, r.parent, &pr, &ps) != 0) return JRN_E_FLOOR;   /* orphan floor */
  pending = s.ram != 0;
  if (pending) {
    rc = jrn_i_pend_last(j, &start, &lastr);
    if (rc || lastr.seq != r.seq) return JRN_E_STATE;
  } else if (!jrn_i_marker_room(j)) {
    j->flush_wanted = 1;                                    /* flush first, then retry */
    return JRN_E_FULL;
  }
  pre = jrn_hash(j);
  rc = apply_record(j, img, &s, &r, 0);
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
  rc = apply_record(j, img, &s, &r, 1);
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
