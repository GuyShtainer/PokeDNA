/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal_int.h -- engine-internal seams between journal.c (format, open, append) and
 * journal_undo.c (undo/redo). Not for callers: include journal.h. */
#ifndef JOURNAL_INT_H
#define JOURNAL_INT_H

#include <stdint.h>

#include "journal.h"

/* Where a record's bytes live: in the pending RAM buffer, or in a segment file. */
typedef struct JrnSrc {
  const uint8_t* ram;   /* non-NULL: the record starts at ram[0]           */
  uint16_t       seg;   /* segment index when ram == NULL                  */
  uint32_t       base;  /* offset of the record inside the segment         */
} JrnSrc;

#define JRN_REC_MAGIC 0x524A4450u   /* 'P','D','J','R' little-endian */

static inline uint16_t jrn_rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t jrn_rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void jrn_wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void jrn_wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* 0 ok, 1 not a record (magic mismatch / zero), 2 malformed header. */
int  jrn_i_hdr_parse(const uint8_t* b, JrnRec* r);
/* Read `n` bytes of a record at record-relative offset `off`. 0 ok. */
int  jrn_i_src_read(const Jrn* j, const JrnSrc* s, uint32_t off, void* buf, uint32_t n);
/* Find a STEP record by seq, pending first. 0 found (r + src filled), JRN_E_FLOOR absent. */
int  jrn_i_locate(Jrn* j, uint32_t seq, JrnRec* r, JrnSrc* src);
/* The last sealed pending record: 0 found. */
int  jrn_i_pend_last(const Jrn* j, uint16_t* start, JrnRec* r);
/* Room for one more span-less marker (after coalescing a trailing cursor marker)? */
int  jrn_i_marker_room(const Jrn* j);
/* Append a marker record to the pending buffer (coalesces a trailing cursor marker). */
int  jrn_i_marker(Jrn* j, uint8_t kind, uint32_t parent, uint32_t aux, uint32_t pre,
                  uint32_t post, const char* name);
/* Drop the last pending record (must be `start`), reclaiming its seq. */
void jrn_i_pend_pop(Jrn* j, uint16_t start);
/* CRC32 of region `region` as the image holds it. 0 ok. */
int  jrn_i_region_crc(const Jrn* j, const JrnImage* img, uint8_t region, uint32_t* out);

#endif /* JOURNAL_INT_H */
